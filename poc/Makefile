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

# lib/: the core. Compiled seeing only itself and the generated geometry, so
# a platform, middleware or host header there is a build error.
LIB_SRC := $(LIB)/util.c $(LIB)/sha256.c $(LIB)/flash.c \
           $(LIB)/image.c $(LIB)/manifest.c $(LIB)/plan.c \
           $(LIB)/flash_view.c $(LIB)/device.c $(LIB)/compress.c
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

all: $(BINS) $(MEM_LD)

$(BUILD) $(OBJ) $(PLATOBJ) $(MWOBJ) $(BINOBJ):
	mkdir -p $@

$(STAMP): FORCE | $(BUILD)
	@echo '$(DEVICE) $(ERASE_SIZE) $(SECTOR_SIZE) $(SECTOR_HASH_LEN)' | cmp -s - $@ || echo '$(DEVICE) $(ERASE_SIZE) $(SECTOR_SIZE) $(SECTOR_HASH_LEN)' >$@

# -I: the generator only reads the TOML; nothing on its path should load
$(GEOM_H) $(MEM_LD) &: $(DEV_TOML) tools/gen_device.py $(STAMP)
	$(PYTHON) -I tools/gen_device.py $(DEV_TOML) --out $(GEN_DIR) $(GEN_ARGS)

$(OBJ)/%.o: $(LIB)/%.c $(GEOM_H) Makefile | $(OBJ)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $(OBJ)/$*.d -iquote $(LIB) -iquote $(GEN_DIR) -c -o $@ $<

$(PLATOBJ)/%.o: $(PLAT)/%.c Makefile | $(PLATOBJ)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $(PLATOBJ)/$*.d -iquote $(PLAT) -iquote $(MW) -c -o $@ $<

$(MWOBJ)/%.o: $(MW)/%.c $(GEOM_H) Makefile | $(MWOBJ)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $(MWOBJ)/$*.d -iquote $(MW) -iquote $(LIB) -iquote $(GEN_DIR) -c -o $@ $<

$(BINOBJ)/%.o: $(SRC)/%.c $(GEOM_H) Makefile | $(BINOBJ)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $(BINOBJ)/$*.d -iquote $(LIB) -iquote $(GEN_DIR) -iquote $(PLAT) -iquote $(MW) -c -o $@ $<

$(LIBCB): $(LIB_OBJ) | $(BUILD)
	$(AR) rcs $@ $^

$(BUILD)/cb-server: $(SRV_OBJ) $(MW_SRV) $(MW_COMMON) $(PLAT_OBJ) $(LIBCB) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ -llz4

$(BUILD)/cb-client: $(CLI_OBJ) $(MW_DEV) $(MW_COMMON) $(PLAT_OBJ) $(LIBCB) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ -llz4

# inspection only: no link, no compressor
$(BUILD)/cb-flash: $(FLS_OBJ) $(MWOBJ)/flash_file.o $(MWOBJ)/host.o $(LIBCB) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

# the decoder eats unchecked bytes: test it under ASan/UBSan
$(BUILD)/compress_test: tests/compress_test.c $(LIB)/compress.c $(MW)/lz4.c $(GEOM_H) | $(BUILD)
	$(CC) $(CFLAGS) -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
		-iquote $(LIB) -iquote $(MW) -iquote $(GEN_DIR) -o $@ tests/compress_test.c $(LIB)/compress.c $(MW)/lz4.c -llz4

# link_order=0: a preloaded library in the environment would stop ASan starting
test-compress: $(BUILD)/compress_test
	ASAN_OPTIONS=verify_asan_link_order=0 $(BUILD)/compress_test

clean:
	rm -rf $(BUILD) run

.PHONY: all clean test-compress FORCE
FORCE:

# must come after every explicit rule: an included .d file's first target
# would otherwise become the default goal instead of 'all'
-include $(ALL_OBJ:.o=.d)
