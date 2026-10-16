CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter
ERASE_SIZE  ?= 1024
SECTOR_SIZE ?= $(ERASE_SIZE)
override CFLAGS += -DCB_ERASE_SIZE=$(ERASE_SIZE) -DCB_SECTOR_SIZE=$(SECTOR_SIZE)

# -iquote, not -I: lib/lz4.h must not shadow liblz4's <lz4.h>
BUILD   := build
SRC     := src
LIB     := lib
PLAT    := platform
OBJ     := $(BUILD)/obj

LIB_SRC := $(LIB)/util.c $(LIB)/sha256.c $(LIB)/flash.c \
           $(LIB)/image.c $(LIB)/manifest.c $(LIB)/frame.c $(LIB)/plan.c \
           $(LIB)/client.c $(LIB)/update_server.c $(LIB)/compress.c $(LIB)/lz4.c
LIB_OBJ := $(LIB_SRC:$(LIB)/%.c=$(OBJ)/%.o)
LIBCB   := $(BUILD)/libcb.a

PLAT_OBJ := $(OBJ)/platform_posix.o

# src/ objects live under their own subdir: src/client.c and lib/client.c
# would otherwise both map to $(OBJ)/client.o
BINOBJ   := $(OBJ)/bin
SRV_OBJ  := $(BINOBJ)/server.o
CLI_OBJ  := $(BINOBJ)/client.o
FLS_OBJ  := $(BINOBJ)/cbflash.o

BINS    := $(BUILD)/cb-server $(BUILD)/cb-client $(BUILD)/cb-flash

DEPFLAGS = -MMD -MP
ALL_OBJ  := $(LIB_OBJ) $(PLAT_OBJ) $(SRV_OBJ) $(CLI_OBJ) $(FLS_OBJ)

# geometry is baked in by -D; the stamp changes only when it does, forcing a rebuild
STAMP := $(BUILD)/geometry.stamp

all: $(BINS)

$(BUILD) $(OBJ) $(BINOBJ):
	mkdir -p $@

$(STAMP): FORCE | $(BUILD)
	@echo '$(ERASE_SIZE) $(SECTOR_SIZE)' | cmp -s - $@ || echo '$(ERASE_SIZE) $(SECTOR_SIZE)' >$@

$(OBJ)/%.o: $(LIB)/%.c $(STAMP) Makefile | $(OBJ)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $(OBJ)/$*.d -iquote $(LIB) -c -o $@ $<

$(BINOBJ)/%.o: $(SRC)/%.c $(STAMP) Makefile | $(BINOBJ)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $(BINOBJ)/$*.d -iquote $(LIB) -iquote $(PLAT) -c -o $@ $<

$(OBJ)/%.o: $(PLAT)/%.c $(STAMP) Makefile | $(OBJ)
	$(CC) $(CFLAGS) $(DEPFLAGS) -MF $(OBJ)/$*.d -iquote $(LIB) -c -o $@ $<

$(LIBCB): $(LIB_OBJ) | $(BUILD)
	$(AR) rcs $@ $^

$(BUILD)/cb-server: $(SRV_OBJ) $(PLAT_OBJ) $(LIBCB) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(SRV_OBJ) $(PLAT_OBJ) $(LIBCB) -llz4 -pthread

$(BUILD)/cb-client: $(CLI_OBJ) $(PLAT_OBJ) $(LIBCB) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(CLI_OBJ) $(PLAT_OBJ) $(LIBCB) -llz4 -pthread

$(BUILD)/cb-flash: $(FLS_OBJ) $(PLAT_OBJ) $(LIBCB) | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(FLS_OBJ) $(PLAT_OBJ) $(LIBCB) -llz4 -pthread

# the decoder eats unchecked bytes: test it under ASan/UBSan
$(BUILD)/compress_test: tests/compress_test.c $(LIB)/compress.c $(LIB)/lz4.c $(STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
		-iquote $(LIB) -o $@ tests/compress_test.c $(LIB)/compress.c $(LIB)/lz4.c -llz4

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
