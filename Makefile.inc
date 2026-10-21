
ifeq ($(OS),Windows_NT)
SHELL := cmd.exe
.SHELLFLAGS := /C
EXEEXT := .exe
else
EXEEXT :=
endif

CC=gcc
CFLAGS += -Wall -g -D__STDC_WANT_LIB_EXT1__=1 -I$(TOP_DIR)src/include/ -I$(TOP_DIR)src/core/include/ -I$(TOP_DIR)src/external/talloc/lib/talloc -I$(TOP_DIR)src/external/talloc/lib/replace

# Source and build dirs
SRC_DIR := .
BUILD_DIR := $(TOP_DIR)/build

# Subdirectories to build if they have Makefiles
# Define only if SUB_DIRS exists and is non-empty
ifdef SUB_DIRS
ifeq ($(OS),Windows_NT)
SUB_MAKE_DIRS := $(SUB_DIRS)
else
SUB_MAKE_DIRS := $(if $(wildcard $(SUB_DIRS)),$(shell find $(SUB_DIRS) -type f -name Makefile -exec dirname {} \;),)
endif
else
SUB_MAKE_DIRS :=
endif

# Collect all .c files in SRC_DIR
SRCS := $(wildcard $(SRC_DIR)/*.c)

# Replace .c with .o and move them into BUILD_DIR
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

all: $(SUB_MAKE_DIRS) $(OBJS)

# Rule to recurse into subdirs if they have a Makefile
$(SUB_MAKE_DIRS):
ifeq ($(OS),Windows_NT)
	@echo ^>^>^> Building in $@
	@$(MAKE) -C $@
else
	echo ">>> Building in $@"; \
	$(MAKE) -C $@;
endif


# Rule to build .o from .c into BUILD_DIR
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Test target for running COME tests in t/ directory
# Usage: make test (from any module directory with a t/ subdirectory)
.PHONY: test clean-test
test:
ifeq ($(OS),Windows_NT)
	@python $(TOP_DIR)/tests/test_runner.py --come --root $(TOP_DIR)
else
	@if [ -d t ]; then \
		echo "Running tests in t/..."; \
		echo "=============================="; \
		passed=0; failed=0; \
		for test in t/*.co; do \
			[ -f "$$test" ] || continue; \
			testname=$$(basename $$test .co); \
			testbin="t/$$testname"; \
			printf "  %-30s " "$$testname"; \
			if $(TOP_DIR)/build/come build $$test -o $$testbin 2>/dev/null; then \
				if $$testbin 2>/dev/null; then \
					echo "✓ PASS"; \
					passed=$$((passed + 1)); \
				else \
					echo "✗ FAIL (runtime)"; \
					failed=$$((failed + 1)); \
				fi; \
			else \
				echo "✗ FAIL (compile)"; \
				failed=$$((failed + 1)); \
			fi; \
		done; \
		echo "=============================="; \
		echo "Results: $$passed passed, $$failed failed"; \
		[ $$failed -eq 0 ]; \
	else \
		echo "No t/ directory found in $$(pwd), skipping tests."; \
		exit 0; \
	fi
endif

# Clean test binaries and generated C files
clean-test:
ifeq ($(OS),Windows_NT)
	@powershell -NoProfile -Command "if (Test-Path 't') { Remove-Item -Force -ErrorAction SilentlyContinue t/*.c,t/*.exe }"
else
	@if [ -d t ]; then \
		echo "Cleaning test artifacts in t/..."; \
		rm -f t/*.c; \
		for test in t/*.co; do \
			[ -f "$$test" ] || continue; \
			testname=$$(basename $$test .co); \
			rm -f "t/$$testname"; \
		done; \
	else \
		echo "No t/ directory found in $$(pwd), skipping test cleanup."; \
	fi
endif

# Ensure build dir exists
$(BUILD_DIR):
ifeq ($(OS),Windows_NT)
	@if not exist "$@" mkdir "$@"
else
	mkdir -p $@
endif

# Clean up
.PHONY: clean $(SUB_DIRS)
clean: clean-test
ifeq ($(OS),Windows_NT)
	-powershell -NoProfile -Command "Remove-Item -Force -ErrorAction SilentlyContinue $(OBJS) $(TARGET)"
else
	rm -f $(OBJS) $(TARGET)
	@for d in $(SUB_DIRS); do \
		if [ -f $$d/Makefile ]; then \
			echo ">>> Cleaning in $$d"; \
			$(MAKE) -C $$d clean; \
		fi; \
	done
endif
