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

$(BUILD_DIR)/std.o: $(BUILD_DIR)/std_manual.o $(BUILD_DIR)/std_gen.o
	$(LD) -r $(BUILD_DIR)/std_manual.o $(BUILD_DIR)/std_gen.o -o $@

$(BUILD_DIR)/std_manual.o: std.c
	$(CC) $(CFLAGS) -c std.c -o $(BUILD_DIR)/std_manual.o

$(BUILD_DIR)/std_gen.o: $(BUILD_DIR)/std.co.c
	$(CC) $(CFLAGS) -c $(BUILD_DIR)/std.co.c -o $(BUILD_DIR)/std_gen.o

$(BUILD_DIR)/std.co.c: std.co $(COME)
	$(COME) genc std.co -o $(BUILD_DIR)/std.co.c

clean:
	rm -f std.co.c std_manual.o std_gen.o $(BUILD_DIR)/std.co.c $(BUILD_DIR)/std_manual.o $(BUILD_DIR)/std_gen.o $(BUILD_DIR)/std.o
