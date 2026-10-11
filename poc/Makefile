CC      ?= gcc
CFLAGS  ?= -O2 -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Wno-unused-parameter
BUILD   := build
SRC     := src

COMMON  := $(SRC)/util.c $(SRC)/md5.c $(SRC)/sha256.c $(SRC)/flash.c \
           $(SRC)/image.c $(SRC)/manifest.c $(SRC)/frame.c $(SRC)/plan.c

BINS    := $(BUILD)/cb-server $(BUILD)/cb-client $(BUILD)/cb-flash

all: $(BINS)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/cb-server: $(SRC)/server.c $(COMMON) | $(BUILD)
	$(CC) $(CFLAGS) -I$(SRC) -o $@ $^

$(BUILD)/cb-client: $(SRC)/client.c $(COMMON) | $(BUILD)
	$(CC) $(CFLAGS) -I$(SRC) -pthread -o $@ $^

$(BUILD)/cb-flash: $(SRC)/cbflash.c $(COMMON) | $(BUILD)
	$(CC) $(CFLAGS) -I$(SRC) -o $@ $^

clean:
	rm -rf $(BUILD) run

.PHONY: all clean
