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
