# Memory Allocator

An educational C memory allocator built on the Unix `sbrk()` interface. It manages a custom heap using a doubly linked list of blocks, page-based growth, coalescing, and a best-fit allocation strategy.

> **Status:** This is a learning project, not a production allocator. It changes the process break, so do not mix it with libc allocation or other code that uses `sbrk()` in the same process.

## Design decisions

- **Keep the tail block free.** The final heap block is maintained as free, so when no other free block fits a request the allocator can grow that tail directly rather than adding a separate empty block.
- **Prefer lower addresses on best-fit ties.** The best-fit search replaces its current candidate only when it finds a strictly smaller fitting block. Equal-size candidates therefore keep the earlier (lower-address) block.
- **Coalesce adjacent free blocks.** Freeing a block merges it with free neighbors. Whole pages at the heap end can then be returned to the system when possible.

## Repository layout

```text
include/mem_alloc.h       Public allocator API
src/mem_alloc.c           Allocator implementation
tests/test_mem_alloc.c    Test suite
Makefile                  Build, test, and cleanup targets
```

## API

Include `mem_alloc.h` to use the allocator:

```c
#include "mem_alloc.h"

void *an_malloc(ssize_t size);
bool  an_free(void *ptr);
void *an_realloc(void *ptr, ssize_t size);
void *an_calloc(size_t nmemb, size_t size);
```

- `an_malloc` allocates a block; non-positive sizes return `NULL`.
- `an_free` releases an allocator-owned block and returns whether it succeeded.
- `an_realloc` resizes a block, in place when possible, and otherwise may move it.
- `an_calloc` allocates zero-initialized memory and checks multiplication overflow.

## Build and test

The Makefile uses GCC by default. From the repository root:

```sh
make          # build the test executable at bin/test_mem_alloc
make test     # build and run the test suite
make lib      # build object/libmem_alloc.a
make check    # build the library, run tests, and rerun tests at -O2
make clean    # remove generated bin/ and object/ directories
```

The test harness runs cases in child processes and checks allocator invariants, allocation and freeing behavior, and stress scenarios. It includes the allocator source directly so it can inspect internal heap state; the test executable should not also link the static library.

## Requirements and limitations

- A GCC-compatible compiler and GNU Make.
- A POSIX-like system with `sbrk()`, `fork()`/`waitpid()`, and `setrlimit()` support. Some environments may not support all tests or `sbrk()`.
- The allocator is intended for single-threaded educational use. Do not treat its internal lock as production-grade thread synchronization.
- Avoid libc allocation calls after the allocator initializes: libc or other libraries may change the program break and interfere with this allocator.
