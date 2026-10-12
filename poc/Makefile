CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter
BUILD   := build
SRC     := src
LIB     := lib
OBJ     := $(BUILD)/obj

# Everything that is not a CLI entry point: manifest/plan/protocol/flash
# primitives plus the device and sender session logic, built once into a
# static library and linked into all three binaries.
LIB_SRC := $(LIB)/util.c $(LIB)/sha256.c $(LIB)/platform_posix.c $(LIB)/flash.c \
           $(LIB)/image.c $(LIB)/manifest.c $(LIB)/frame.c $(LIB)/plan.c \
           $(LIB)/client.c $(LIB)/update_server.c
LIB_OBJ := $(LIB_SRC:$(LIB)/%.c=$(OBJ)/%.o)
LIBCB   := $(BUILD)/libcb.a

BINS    := $(BUILD)/cb-server $(BUILD)/cb-client $(BUILD)/cb-flash

all: $(BINS)

$(BUILD) $(OBJ):
	mkdir -p $@

$(OBJ)/%.o: $(LIB)/%.c | $(OBJ)
	$(CC) $(CFLAGS) -I$(LIB) -c -o $@ $<

$(LIBCB): $(LIB_OBJ) | $(BUILD)
	$(AR) rcs $@ $^

$(BUILD)/cb-server: $(SRC)/server.c $(LIBCB) | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -o $@ $(SRC)/server.c $(LIBCB)

$(BUILD)/cb-client: $(SRC)/client.c $(LIBCB) | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -pthread -o $@ $(SRC)/client.c $(LIBCB)

$(BUILD)/cb-flash: $(SRC)/cbflash.c $(LIBCB) | $(BUILD)
	$(CC) $(CFLAGS) -I$(LIB) -o $@ $(SRC)/cbflash.c $(LIBCB)

clean:
	rm -rf $(BUILD) run

.PHONY: all clean
