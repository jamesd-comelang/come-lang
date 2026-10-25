TOP_DIR=../../
CC=gcc
CFLAGS=-Wall -g -O2 -flto -ffat-lto-objects -I$(TOP_DIR)src/include/ -I$(TOP_DIR)src/core/include/
BUILD_DIR=$(TOP_DIR)/build
COME=$(BUILD_DIR)/come

# We need to link both the manual C code (string.c) and the generated C code (string.co.c)
# into a single object file for the linker.
all: $(BUILD_DIR)/string.o

$(BUILD_DIR)/string.o: $(BUILD_DIR)/string_manual.o $(BUILD_DIR)/string_gen.o
	# perf2 B: plain `ld -r` drops fatness (LTO sections survive
	# but no usable machine code, breaking -fno-lto links).
	# `gcc -r` with LTO flags preserves both code and bytecode.
	$(CC) -flto -ffat-lto-objects -r $(BUILD_DIR)/string_manual.o $(BUILD_DIR)/string_gen.o -o $@

$(BUILD_DIR)/string_manual.o: string.c
	$(CC) $(CFLAGS) -c string.c -o $(BUILD_DIR)/string_manual.o

$(BUILD_DIR)/string_gen.o: $(BUILD_DIR)/string.co.c
	$(CC) $(CFLAGS) -c $(BUILD_DIR)/string.co.c -o $(BUILD_DIR)/string_gen.o

$(BUILD_DIR)/string.co.c: string.co $(COME)
	cd $(TOP_DIR) && ./build/come genc src/string/string.co -o build/string.co.c

clean:
	rm -f string.co.c string_manual.o string_gen.o $(BUILD_DIR)/string.co.c $(BUILD_DIR)/string_manual.o $(BUILD_DIR)/string_gen.o $(BUILD_DIR)/string.o

.PHONY: all clean
