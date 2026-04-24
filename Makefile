# Makefile - brc v0.0.1a
# Compilador da linguagem BR, escrito em C padrao (ISO C17).

CC       := gcc
CSTD     := -std=c17
WARN     := -Wall -Wextra -Wpedantic -Werror -Wshadow -Wstrict-prototypes \
            -Wmissing-prototypes -Wold-style-definition
OPT      := -O2
DEBUG    := -g
CFLAGS   := $(CSTD) $(WARN) $(OPT) $(DEBUG) -Iinclude -MMD -MP
LDFLAGS  :=

SRC_DIR  := src
OBJ_DIR  := build
BIN      := brc

SRCS     := $(wildcard $(SRC_DIR)/*.c)
OBJS     := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

TEST_DIR := tests
TEST_SRCS := $(wildcard $(TEST_DIR)/*.br)

.PHONY: all clean test memcheck dirs

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | dirs
	$(CC) $(CFLAGS) -c -o $@ $<

# Inclui os arquivos .d gerados pelo -MMD, para recompilar automaticamente
# objetos quando um header incluido for modificado.
-include $(OBJS:.o=.d)

dirs:
	@mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(BIN) $(TEST_DIR)/*.s $(TEST_DIR)/*.o \
	       $(TEST_DIR)/*.out $(TEST_DIR)/*.err $(TEST_DIR)/*.stdout $(TEST_DIR)/*.diff

test: $(BIN)
	@./scripts/run_tests.sh

memcheck: $(BIN)
	@for t in $(TEST_SRCS); do \
		echo ">>> valgrind em $$t"; \
		valgrind --error-exitcode=1 --leak-check=full --show-leak-kinds=all \
		         --errors-for-leak-kinds=all -q ./$(BIN) $$t -o /tmp/brc_memcheck.out \
		         || exit 1; \
	done
	@echo "memcheck OK"
