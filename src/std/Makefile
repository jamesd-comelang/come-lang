TOP_DIR=../../
CC=gcc
CFLAGS=-Wall -g -I$(TOP_DIR)src/include/ -I$(TOP_DIR)src/core/include/ -I$(TOP_DIR)external/talloc/lib/talloc -I$(TOP_DIR)external/talloc/lib/replace
BUILD_DIR=$(TOP_DIR)/build
COME=$(BUILD_DIR)/come

# We need to link both the manual C code (std.c) and the generated C code (std.co.c)
# into a single object file for the linker.
# We CANNOT just use $(CC) -c std.c -o std.o because we need to combine them.
# So we use `ld -r` (relocatable link) to combine them into ../../build/std.o

all: $(BUILD_DIR)/std.o

$(BUILD_DIR)/std.o: std_manual.o std_gen.o
	$(LD) -r std_manual.o std_gen.o -o $@

std_manual.o: std.c
	$(CC) $(CFLAGS) -c std.c -o std_manual.o

std_gen.o: std.co.c
	$(CC) $(CFLAGS) -c std.co.c -o std_gen.o

std.co.c: std.co $(COME)
	$(COME) genc std.co -o std.co.c

clean:
	rm -f std.co.c std_manual.o std_gen.o $(BUILD_DIR)/std.o
