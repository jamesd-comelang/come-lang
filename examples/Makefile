# Examples Makefile
# Build COME examples using the compiler

# Disable built-in rules
.SUFFIXES:

# Compiler path (relative to examples/)
COME = ../build/come
PROJECT_ROOT = ..

# Find all .co files
CO_FILES = $(wildcard *.co)
BINARIES = $(patsubst %.co,%,$(CO_FILES))

# Default: build all examples
all: $(BINARIES)

# Pattern rule: build each example
# Run from project root to ensure correct relative paths
%: %.co $(COME)
	@echo "Building example $<"
	@cd $(PROJECT_ROOT) && ./build/come build examples/$< -o examples/$@

# Run all examples
run: all
	@for bin in $(BINARIES); do \
		echo "Running $$bin"; \
		./$$bin; \
	done

# Clean built examples
clean:
	@echo "Cleaning examples..."
	-rm -f $(BINARIES)
	-rm -f *.c

.PHONY: all run clean
