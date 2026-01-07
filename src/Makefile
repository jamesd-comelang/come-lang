TOP_DIR=../
# Subdirectories to build if they have Makefiles (core modules that don't need CO compiler)
SUB_DIRS := core mem array map

include $(TOP_DIR)/Makefile.inc

# Final binary
TARGET := $(BUILD_DIR)/come

# Link all objects into final binary
.PHONY: $(SUB_DIRS)
# Explicitly list compiler objects to avoid picking up tests/examples in the build dir
COMPILER_OBJS := $(addprefix $(BUILD_DIR)/, come_compiler.o codegen.o lexer.o parser.o utils.o array.o map.o talloc.o talloc_lib.o)

$(TARGET): $(SUB_MAKE_DIRS)
	$(CC) $(CFLAGS) -o $@ $(COMPILER_OBJS) -ldl

# Build modules that require come compiler
std: $(TARGET)
	$(MAKE) -C std

string: $(TARGET)
	$(MAKE) -C string

all: $(SUB_MAKE_DIRS) $(TARGET) std string

clean: clean-std clean-string

clean-std:
	$(MAKE) -C std clean

clean-string:
	$(MAKE) -C string clean
