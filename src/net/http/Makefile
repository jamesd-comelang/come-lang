TOP_DIR=../../../

CFLAGS += -I$(TOP_DIR)/external/llhttp/build

include $(TOP_DIR)/Makefile.inc

LLHTTP_SRC = $(TOP_DIR)/external/llhttp/build/c/llhttp.c
LLHTTP_OBJ = $(BUILD_DIR)/llhttp.o

$(LLHTTP_OBJ): $(LLHTTP_SRC) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

all: $(LLHTTP_OBJ)
