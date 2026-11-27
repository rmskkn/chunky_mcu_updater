CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter
PYTHON  ?= python3

# Device catalog entry: device/$(DEVICE)/device.toml. tools/gen_device.py
# turns it into build/device/geometry.h, compiled into every object, and
# build/device/memory.ld. ERASE_SIZE / SECTOR_SIZE, when set, override the
# TOML (the test matrix runs one part at several geometries); a sector
# defaults to the erase unit when only ERASE_SIZE is given.
DEVICE      ?= stm32f103cb
DEV_TOML    := device/$(DEVICE)/device.toml
ifeq ($(wildcard $(DEV_TOML)),)
$(error no device catalog entry $(DEV_TOML))
endif
SECTOR_SIZE ?= $(ERASE_SIZE)
# Bytes of SHA-256 kept per sector hash (lib/protocol.h; default 8). Wire
# format: server and device must be built with the same value.
SECTOR_HASH_LEN ?=
override CFLAGS += $(if $(SECTOR_HASH_LEN),-DCB_SECTOR_HASH_LEN=$(SECTOR_HASH_LEN))
GEN_ARGS    := $(if $(ERASE_SIZE),--erase-size $(ERASE_SIZE)) \
               $(if $(SECTOR_SIZE),--sector-size $(SECTOR_SIZE))

# -iquote, not -I: middleware/lz4.h must not shadow liblz4's <lz4.h>
BUILD   := build
SRC     := src
LIB     := lib
PLAT    := platform
MW      := middleware
OBJ     := $(BUILD)/obj
GEN_DIR := $(BUILD)/device
GEOM_H  := $(GEN_DIR)/geometry.h
MEM_LD  := $(GEN_DIR)/memory.ld

# lib/: the core, sees only itself and the generated geometry; any outside header here is a build error.
LIB_SRC := $(LIB)/util.c $(LIB)/sha256.c $(LIB)/flash.c \
           $(LIB)/bootloader.c $(LIB)/device.c \
           $(LIB)/compress.c $(LIB)/format2.c $(LIB)/format2_geom.c \
           $(LIB)/format2_image.c $(LIB)/format2_plan.c $(LIB)/format2_manifest.c
LIB_OBJ := $(LIB_SRC:$(LIB)/%.c=$(OBJ)/%.o)
LIBCB   := $(BUILD)/libcb.a

# platform/: the host's transport (TCP)
PLATOBJ  := $(OBJ)/plat
PLAT_OBJ := $(PLATOBJ)/platform_posix.o

# middleware/: what carries the core between a device and a server on a
# host: framing, both sessions, the file-backed flash port, host services,
# and the LZ4 binding
MWOBJ     := $(OBJ)/mw
MW_COMMON := $(MWOBJ)/frame.o $(MWOBJ)/host.o $(MWOBJ)/lz4.o
MW_DEV    := $(MWOBJ)/flash_file.o $(MWOBJ)/client_session.o
MW_SRV    := $(MWOBJ)/server_session.o
MW_OBJ    := $(MW_COMMON) $(MW_DEV) $(MW_SRV)

# src/: the CLIs
BINOBJ   := $(OBJ)/bin
SRV_OBJ  := $(BINOBJ)/server.o
CLI_OBJ  := $(BINOBJ)/client.o
FLS_OBJ  := $(BINOBJ)/cbflash.o

BINS    := $(BUILD)/cb-server $(BUILD)/cb-client $(BUILD)/cb-flash

DEPFLAGS = -MMD -MP
ALL_OBJ  := $(LIB_OBJ) $(PLAT_OBJ) $(MW_OBJ) $(SRV_OBJ) $(CLI_OBJ) $(FLS_OBJ)

# device and geometry overrides are baked in at compile time; the stamp
# changes only when they do, forcing a regeneration and a rebuild
STAMP := $(BUILD)/geometry.stamp

all: $(BINS) $(BUILD)/auth_test $(MEM_LD)

$(BUILD) $(OBJ) $(PLATOBJ) $(MWOBJ) $(BINOBJ):
	mkdir -p $@

$(STAMP): FORCE | $(BUILD)
	@echo '$(DEVICE) $(ERASE_SIZE) $(SECTOR_SIZE) $(SECTOR_HASH_LEN)' | cmp -s - $@ || echo '$(DEVICE) $(ERASE_SIZE) $(SECTOR_SIZE) $(SECTOR_HASH_LEN)' >$@

# -I: the generator only reads the TOML; nothing on its path should load
$(GEOM_H) $(MEM_LD) &: $(DEV_TOML) tools/gen_device.py $(STAMP)
	$(PYTHON) -I tools/gen_device.py $(DEV_TOML) --out $(GEN_DIR) $(GEN_ARGS)

# -iquote $(FMT_C): needed by format2_image.c/_plan.c/_manifest.c's chunky_format.h include; harmless on the rest, so kept on the shared rule instead of split per file.
$(OBJ)/%.o: $(LIB)/%.c $(GEOM_H) Makefile | $(OBJ)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $(OBJ)/$*.d -iquote $(LIB) -iquote $(GEN_DIR) -iquote $(FMT_C) -c -o $@ $<

$(PLATOBJ)/%.o: $(PLAT)/%.c Makefile | $(PLATOBJ)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $(PLATOBJ)/$*.d -iquote $(PLAT) -iquote $(MW) -c -o $@ $<

# -iquote $(FMT_C): server_session.c/client_session.c need chunky_format.h via lib/device.h/format2_plan.h, same reason as the lib/*.c rule above.
$(MWOBJ)/%.o: $(MW)/%.c $(GEOM_H) Makefile | $(MWOBJ)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $(MWOBJ)/$*.d -iquote $(MW) -iquote $(LIB) -iquote $(GEN_DIR) -iquote $(FMT_C) -c -o $@ $<

# -iquote $(FMT_C): client.c/cbflash.c read format2 descriptors/manifests directly, same chunky_format.h chain, same reason as above.
$(BINOBJ)/%.o: $(SRC)/%.c $(GEOM_H) Makefile | $(BINOBJ)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $(BINOBJ)/$*.d -iquote $(LIB) -iquote $(GEN_DIR) -iquote $(PLAT) -iquote $(MW) -iquote $(FMT_C) -c -o $@ $<

$(LIBCB): $(LIB_OBJ) | $(BUILD)
	$(AR) rcs $@ $^

# $(MWOBJ)/flash_file.o: cb-server never opens a device, but it's linked in anyway to resolve flash_* symbols that format2_plan.c's translation unit (pulled in by server_session.c) needs.
$(BUILD)/cb-server: $(SRV_OBJ) $(MW_SRV) $(MWOBJ)/flash_file.o $(MW_COMMON) $(PLAT_OBJ) $(LIBCB) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ -llz4

$(BUILD)/cb-client: $(CLI_OBJ) $(MW_DEV) $(MW_COMMON) $(PLAT_OBJ) $(LIBCB) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ -llz4

# inspection only: no link, no compressor
$(BUILD)/cb-flash: $(FLS_OBJ) $(MWOBJ)/flash_file.o $(MWOBJ)/host.o $(LIBCB) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

# drives the session and the bootloader message by message (tests/case_auth.sh).
# -iquote $(FMT_C): auth_test.c reads format2 descriptors/plans directly, same chunky_format.h chain as above.
$(BUILD)/auth_test: tests/auth_test.c $(MWOBJ)/flash_file.o $(MWOBJ)/host.o $(LIBCB) $(GEOM_H) | $(BUILD)
	$(CC) $(CFLAGS) -iquote $(LIB) -iquote $(MW) -iquote $(SRC) -iquote $(GEN_DIR) -iquote $(FMT_C) -o $@ \
		tests/auth_test.c $(MWOBJ)/flash_file.o $(MWOBJ)/host.o $(LIBCB)

# the decoder eats unchecked bytes: test it under ASan/UBSan
$(BUILD)/compress_test: tests/compress_test.c $(LIB)/compress.c $(MW)/lz4.c $(GEOM_H) | $(BUILD)
	$(CC) $(CFLAGS) -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
		-iquote $(LIB) -iquote $(MW) -iquote $(GEN_DIR) -o $@ tests/compress_test.c $(LIB)/compress.c $(MW)/lz4.c -llz4

# link_order=0: a preloaded library in the environment would stop ASan starting
test-compress: $(BUILD)/compress_test
	ASAN_OPTIONS=verify_asan_link_order=0 $(BUILD)/compress_test

# rust/chunky_format/c: the generated C99 reader for chunky's format 2 (SPEC.md),
# vendored unmodified from github.com/rmskkn/chunky_mcu_updater PR #5. Needs no
# Rust toolchain (header-only) and no geometry (format 2 is a different wire
# format from this repo's own, DESIGN.md sections 4-6; lib/format2.c only reads
# buffers it is handed, so it does not depend on $(GEOM_H)).
FMT_C      := rust/chunky_format/c
FMT_CFLAGS := -Werror -pedantic -Wconversion -Wsign-conversion

# format2_test feeds lib/format2.c every shorter prefix of the golden
# vectors: under ASan/UBSan, like compress_test, so a read past one fails
$(BUILD)/format2_test: tests/format2_test.c $(LIB)/format2.c $(LIB)/format2.h $(LIB)/sha256.c $(LIB)/sha256.h \
                      $(FMT_C)/chunky_format.h $(FMT_C)/golden.h Makefile | $(BUILD)
	$(CC) $(CFLAGS) $(FMT_CFLAGS) -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
		-iquote $(FMT_C) -iquote $(LIB) -o $@ tests/format2_test.c $(LIB)/format2.c $(LIB)/sha256.c

test-format2: $(BUILD)/format2_test
	ASAN_OPTIONS=verify_asan_link_order=0 $(BUILD)/format2_test

# format2-tools: one CLI, two subcommands (dump, check), dispatched on
# argv[1] -- dumps the vendored golden vectors to real .bin files, and
# verifies a format-2 plan file through lib/format2.c's real checks (seal,
# per-step hash, order) rather than a structural parse only. A standalone
# tool, not a test, so it lives in tools/ (like gen_device.py) rather than
# tests/. Needs no $(GEOM_H): format 2 is not this repo's own wire format
# (see above).
$(BUILD)/format2: tools/format2.c $(LIB)/format2.c $(LIB)/format2.h $(LIB)/sha256.c $(LIB)/sha256.h \
                  $(FMT_C)/chunky_format.h $(FMT_C)/golden.h Makefile | $(BUILD)
	$(CC) $(CFLAGS) $(FMT_CFLAGS) -iquote $(FMT_C) -iquote $(LIB) -o $@ \
		tools/format2.c $(LIB)/format2.c $(LIB)/sha256.c

format2-tools: $(BUILD)/format2
# rust/: the wire format of SPEC.md as Rust types (chunky_format), the C reader
# generated from them (chunky_format.h) and the golden vectors. Only rust,
# rust-test, regen and check-regen need cargo; vectors and example are plain C.
CARGO      ?= cargo
RUST       := rust
FMT_C      := $(RUST)/chunky_format/c
FMT_VEC    := $(RUST)/chunky_format/vectors
FMT_CFLAGS := -Werror -pedantic -Wconversion -Wsign-conversion
FMT_GEN    := $(CARGO) run -q --manifest-path $(RUST)/Cargo.toml -p chunky_format_gen --

rust:
	$(CARGO) build --manifest-path $(RUST)/Cargo.toml

rust-test:
	$(CARGO) test --manifest-path $(RUST)/Cargo.toml

# the C reader against the golden vectors, signed and unsigned, and every
# shorter buffer refused, under ASan/UBSan so a read past a buffer fails
$(BUILD)/chunky_format_test: $(FMT_C)/test.c $(FMT_C)/chunky_format.h $(FMT_C)/golden.h | $(BUILD)
	$(CC) $(CFLAGS) $(FMT_CFLAGS) -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
		-iquote $(FMT_C) -o $@ $<

vectors: $(BUILD)/chunky_format_test
	ASAN_OPTIONS=verify_asan_link_order=0 $(BUILD)/chunky_format_test

# how a C peer reads a plan: build/read_plan PLAN.bin
$(BUILD)/read_plan: $(FMT_C)/read_plan.c $(FMT_C)/chunky_format.h | $(BUILD)
	$(CC) $(CFLAGS) $(FMT_CFLAGS) -iquote $(FMT_C) -o $@ $<

example: $(BUILD)/read_plan
	$(BUILD)/read_plan $(FMT_VEC)/signed/plan.bin
	$(BUILD)/read_plan $(FMT_VEC)/unsigned/plan.bin

# rewrites chunky_format.h and its stamp; nothing when its inputs did not change
regen:
	$(FMT_GEN) regen

# fails when the committed chunky_format.h is stale
check-regen:
	$(FMT_GEN) check

# optional: a pre-commit hook that runs check-regen when a commit touches rust/
hooks:
	install -m 755 scripts/pre-commit "$$(git rev-parse --git-path hooks)/pre-commit"

clean:
	rm -rf $(BUILD) run

.PHONY: all clean test-compress FORCE rust rust-test vectors example regen check-regen hooks
FORCE:

# must come after every explicit rule: an included .d file's first target
# would otherwise become the default goal instead of 'all'
-include $(ALL_OBJ:.o=.d)
