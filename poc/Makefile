CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter
BUILD   := build
SRC     := src
LIB     := lib
OBJ     := $(BUILD)/obj

# Everything that is not a CLI entry point and not the backing-store IO:
# manifest/plan/protocol/flash primitives plus the device and sender
# session logic, built once into a static library and linked into all
# three binaries.
LIB_SRC := $(LIB)/util.c $(LIB)/sha256.c $(LIB)/flash.c \
           $(LIB)/image.c $(LIB)/manifest.c $(LIB)/frame.c $(LIB)/plan.c \
           $(LIB)/client.c $(LIB)/update_server.c
LIB_OBJ := $(LIB_SRC:$(LIB)/%.c=$(OBJ)/%.o)
LIBCB   := $(BUILD)/libcb.a

# The backing-store IO (poc/lib/platform.h) is the one seam each binary
# picks for itself rather than sharing through libcb.a -- today every
# binary is a host PoC tool and picks platform_posix.o, but a real target
# binary would link its own platform_<mcu>.o here instead.
PLAT_OBJ := $(OBJ)/platform_posix.o

BINS    := $(BUILD)/cb-server $(BUILD)/cb-client $(BUILD)/cb-flash

all: $(BINS)

$(BUILD) $(OBJ):
	mkdir -p $@

$(OBJ)/%.o: $(LIB)/%.c | $(OBJ)
	$(CC) $(CFLAGS) -I$(LIB) -c -o $@ $<

$(LIBCB): $(LIB_OBJ) | $(BUILD)
	$(AR) rcs $@ $^

$(BUILD)/cb-server: $(SRC)/server.c $(PLAT_OBJ) $(LIBCB) | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -o $@ $(SRC)/server.c $(PLAT_OBJ) $(LIBCB)

$(BUILD)/cb-client: $(SRC)/client.c $(PLAT_OBJ) $(LIBCB) | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -pthread -o $@ $(SRC)/client.c $(PLAT_OBJ) $(LIBCB)

$(BUILD)/cb-flash: $(SRC)/cbflash.c $(PLAT_OBJ) $(LIBCB) | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -o $@ $(SRC)/cbflash.c $(PLAT_OBJ) $(LIBCB)

clean:
	rm -rf $(BUILD) run

.PHONY: all clean
