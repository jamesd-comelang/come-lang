TOP_DIR=../../
# Subdirectories to build if they have Makefiles
SUB_DIRS :=

include $(TOP_DIR)/Makefile.inc

# Just build objects, do not link.
# The memory framework is memory.c in this directory (built via OBJS).

# COIL-158: this module links into every user program, so
# it builds optimized. The compiler core sources stay at
# the default level: -O2 surfaces pre-existing truncation
# warnings in codegen/parser that are out of scope here.
CFLAGS += -O2
# Design 6 follow-up (redesign): full LTO needs LTO info
# in runtime objects too. Measured ~33% on the tokenizer
# with COME_CFLAGS="-O2 -flto" (0.57s -> 0.38s).
CFLAGS += -flto
# perf2 B: fat objects (machine code + bytecode). Slim objects
# forced auto-LTO at -O0 in default links (the plugin claims
# LTO sections even without -flto), so the runtime ran at -O0
# in default builds. Fat + -fno-lto on non-LTO links (driver)
# gives -O2 runtime code there; --lto links still use bytecode.
CFLAGS += -ffat-lto-objects
