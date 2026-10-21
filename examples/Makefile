# Examples Makefile
# Build COME examples using the compiler

# Disable built-in rules
.SUFFIXES:

# Compiler path (relative to examples/)
ifeq ($(OS),Windows_NT)
SHELL := cmd.exe
.SHELLFLAGS := /C
EXEEXT := .exe
COME = ../build/come$(EXEEXT)
COME_FROM_ROOT = build\come$(EXEEXT)
else
EXEEXT :=
COME = ../build/come
COME_FROM_ROOT = ./build/come
endif
PROJECT_ROOT = ..

# Find all .co files
CO_FILES = $(wildcard *.co)
BINARIES = $(patsubst %.co,%$(EXEEXT),$(CO_FILES))

# Default: build all examples
all: $(BINARIES)

# Pattern rule: build each example
# Run from project root to ensure correct relative paths
%$(EXEEXT): %.co $(COME)
	@echo "Building example $<"
	@cd $(PROJECT_ROOT) && $(COME_FROM_ROOT) build examples/$< -o examples/$@

# Run all examples
run: all
ifeq ($(OS),Windows_NT)
	@for %%b in ($(BINARIES)) do @echo Running %%b && %%b
else
	@for bin in $(BINARIES); do \
		echo "Running $$bin"; \
		./$$bin; \
	done
endif

# Clean built examples
clean:
	@echo "Cleaning examples..."
ifeq ($(OS),Windows_NT)
	-del /Q /F *.exe *.c 2>NUL
else
	-rm -f $(BINARIES)
	-rm -f *.c
endif

.PHONY: all run clean
