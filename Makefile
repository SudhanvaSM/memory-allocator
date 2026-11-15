CC       = gcc
CFLAGS   ?= -g -O0
STD      := -std=gnu11
WARN     := -Wall -Wextra
CPPFLAGS := -Iinclude -Isrc -MMD -MP

TEST_BIN := bin/test_mem_alloc
TEST_OBJ := object/test_mem_alloc.o
LIB      := object/libmem_alloc.a

all: $(TEST_BIN)

# The test file #includes src/mem_alloc.c so it can inspect internals; do NOT link the library into it.
$(TEST_BIN): $(TEST_OBJ) | bin
	$(CC) $(CFLAGS) $^ -o $@

object/%.o: tests/%.c | object
	$(CC) $(STD) $(WARN) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

# Library build: compiled separately, with stricter warnings than the tests.
object/%.o: src/%.c | object
	$(CC) $(STD) $(WARN) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

object/mem_alloc.o: WARN += -Wpedantic -Wshadow -Wconversion -Wsign-conversion

lib: $(LIB)
$(LIB): object/mem_alloc.o
	ar rcs $@ $^

bin object:
	mkdir -p $@

test: $(TEST_BIN)
	./$(TEST_BIN)

# Rebuild from scratch at -O2 and run again.
test-o2:
	$(MAKE) --no-print-directory clean
	$(MAKE) --no-print-directory test CFLAGS="-g -O2"

# Everything CI will run.
check: lib test test-o2

clean:
	rm -rf object bin

-include $(wildcard object/*.d)

.PHONY: all lib test test-o2 check clean