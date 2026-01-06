
CC=gcc
CFLAGS += -Wall -g -D__STDC_WANT_LIB_EXT1__=1 -I$(TOP_DIR)src/include/ -I$(TOP_DIR)src/core/include/ -I$(TOP_DIR)src/external/talloc/lib/talloc -I$(TOP_DIR)src/external/talloc/lib/replace

# Source and build dirs
SRC_DIR := .
BUILD_DIR := $(TOP_DIR)/build

# Subdirectories to build if they have Makefiles
# Define only if SUB_DIRS exists and is non-empty
ifdef SUB_DIRS
SUB_MAKE_DIRS := $(if $(wildcard $(SUB_DIRS)),$(shell find $(SUB_DIRS) -type f -name Makefile -exec dirname {} \;),)
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
	echo ">>> Building in $@"; \
	$(MAKE) -C $@; \


# Rule to build .o from .c into BUILD_DIR
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Test target for running COME tests in t/ directory
# Usage: make test (from any module directory with a t/ subdirectory)
.PHONY: test clean-test
test:
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

# Clean test binaries and generated C files
clean-test:
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

# Ensure build dir exists
$(BUILD_DIR):
	mkdir -p $@

# Clean up
.PHONY: clean $(SUB_DIRS)
clean: clean-test
	rm -f $(OBJS) $(TARGET)
	@for d in $(SUB_DIRS); do \
		if [ -f $$d/Makefile ]; then \
			echo ">>> Cleaning in $$d"; \
			$(MAKE) -C $$d clean; \
		fi; \
	done
