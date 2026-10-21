TOP_DIR=../
# Subdirectories to build if they have Makefiles (core modules that don't need CO compiler)
SUB_DIRS := core mem array map

include $(TOP_DIR)/Makefile.inc

ifeq ($(OS),Windows_NT)
DL_LIB :=
else
DL_LIB := -ldl
endif

# Final binary
TARGET := $(BUILD_DIR)/come$(EXEEXT)

# Link all objects into final binary
.PHONY: $(SUB_DIRS)
# Explicitly list compiler objects to avoid picking up tests/examples in the build dir
ifeq ($(OS),Windows_NT)
COMPILER_OBJS := $(addprefix $(BUILD_DIR)/, come_compiler.o codegen.o lexer.o parser.o utils.o array.o map.o talloc.o)
else
COMPILER_OBJS := $(addprefix $(BUILD_DIR)/, come_compiler.o codegen.o lexer.o parser.o utils.o array.o map.o talloc.o talloc_lib.o)
endif

$(TARGET): $(SUB_MAKE_DIRS)
	$(CC) $(CFLAGS) -o $@ $(COMPILER_OBJS) $(DL_LIB)

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
