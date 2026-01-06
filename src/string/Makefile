TOP_DIR=../../
CC=gcc
CFLAGS=-Wall -g -I$(TOP_DIR)src/include/ -I$(TOP_DIR)src/core/include/ -I$(TOP_DIR)external/talloc/lib/talloc -I$(TOP_DIR)external/talloc/lib/replace
BUILD_DIR=$(TOP_DIR)/build
COME=$(BUILD_DIR)/come

# We need to link both the manual C code (string.c) and the generated C code (string.co.c)
# into a single object file for the linker.
all: $(BUILD_DIR)/string.o

$(BUILD_DIR)/string.o: string_manual.o string_gen.o
	$(LD) -r string_manual.o string_gen.o -o $@

string_manual.o: string.c
	$(CC) $(CFLAGS) -c string.c -o string_manual.o

string_gen.o: string.co.c
	$(CC) $(CFLAGS) -c string.co.c -o string_gen.o

string.co.c: string.co $(COME)
	$(COME) genc string.co -o string.co.c

clean:
	rm -f string.co.c string_manual.o string_gen.o $(BUILD_DIR)/string.o

.PHONY: all clean
