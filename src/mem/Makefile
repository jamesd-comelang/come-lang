TOP_DIR=../../
# Subdirectories to build if they have Makefiles
SUB_DIRS := 

include $(TOP_DIR)/Makefile.inc

# Just build objects, do not link
# Add talloc.o from external
TALLOC_SRC = $(TOP_DIR)/src/external/talloc/lib/talloc/talloc.c
TALLOC_OBJ = $(BUILD_DIR)/talloc_lib.o

all: $(OBJS) $(TALLOC_OBJ)

$(TALLOC_OBJ): $(TALLOC_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
