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
OBJ     := $(BUILD)/obj

LIB_SRC := $(LIB)/util.c $(LIB)/sha256.c $(LIB)/flash.c \
           $(LIB)/image.c $(LIB)/manifest.c $(LIB)/frame.c $(LIB)/plan.c \
           $(LIB)/client.c $(LIB)/update_server.c $(LIB)/compress.c $(LIB)/lz4.c
LIB_OBJ := $(LIB_SRC:$(LIB)/%.c=$(OBJ)/%.o)
LIBCB   := $(BUILD)/libcb.a

PLAT_OBJ := $(OBJ)/platform_posix.o

BINS    := $(BUILD)/cb-server $(BUILD)/cb-client $(BUILD)/cb-flash

# geometry is baked in by -D; the stamp changes only when it does, forcing a rebuild
STAMP := $(BUILD)/geometry.stamp

all: $(BINS)

$(BUILD) $(OBJ):
	mkdir -p $@

$(STAMP): FORCE | $(BUILD)
	@echo '$(ERASE_SIZE) $(SECTOR_SIZE)' | cmp -s - $@ || echo '$(ERASE_SIZE) $(SECTOR_SIZE)' >$@

$(OBJ)/%.o: $(LIB)/%.c $(STAMP) | $(OBJ)
	$(CC) $(CFLAGS) -iquote $(LIB) -c -o $@ $<

$(OBJ)/%.o: $(SRC)/%.c $(STAMP) | $(OBJ)
	$(CC) $(CFLAGS) -iquote $(LIB) -c -o $@ $<

$(LIBCB): $(LIB_OBJ) | $(BUILD)
	$(AR) rcs $@ $^

$(BUILD)/cb-server: $(SRC)/server.c $(PLAT_OBJ) $(LIBCB) $(STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -iquote $(LIB) -o $@ $(SRC)/server.c $(PLAT_OBJ) $(LIBCB) -llz4

$(BUILD)/cb-client: $(SRC)/client.c $(PLAT_OBJ) $(LIBCB) $(STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -iquote $(LIB) -pthread -o $@ $(SRC)/client.c $(PLAT_OBJ) $(LIBCB) -llz4

$(BUILD)/cb-flash: $(SRC)/cbflash.c $(PLAT_OBJ) $(LIBCB) $(STAMP) | $(BUILD)
	$(CC) $(CFLAGS) -iquote $(LIB) -o $@ $(SRC)/cbflash.c $(PLAT_OBJ) $(LIBCB) -llz4

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
