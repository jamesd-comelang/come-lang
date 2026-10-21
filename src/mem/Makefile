TOP_DIR=../../
# Subdirectories to build if they have Makefiles
SUB_DIRS := 

include $(TOP_DIR)/Makefile.inc

# Just build objects, do not link
# Add upstream talloc only on POSIX. Windows uses the lightweight wrapper in
# src/mem/talloc.c and does not need libdl or the external talloc sources.
TALLOC_SRC = $(TOP_DIR)/src/external/talloc/lib/talloc/talloc.c
TALLOC_OBJ = $(BUILD_DIR)/talloc_lib.o

ifeq ($(OS),Windows_NT)
all: $(OBJS)
else
all: $(OBJS) $(TALLOC_OBJ)
endif

$(TALLOC_OBJ): $(TALLOC_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
