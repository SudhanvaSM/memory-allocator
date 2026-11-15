# Memory Allocator

A small C allocator built around the Unix `sbrk()` interface. It implements a custom heap manager with a doubly linked list of blocks, page-based growth, coalescing, and a best-fit allocation strategy.

> Status: This project is educational and intentionally small. It is not a production allocator and should not be mixed with libc `malloc` or other code that changes the process break in the same process.

## Design goals and decisions

This allocator keeps a very specific heap layout so allocation logic stays simple and predictable:

- The final block in the heap is always kept free. This means allocation does not need a separate "is the last block free?" branch: if no other free block matches, the allocator can grow the tail block directly instead of creating an extra temporary free block.
- Best-fit uses a strict `>` tie-break on block length, so when two blocks are the same size, the lower-address block wins. This keeps allocations clustered toward the beginning of the heap and helps preserve the tail invariant.
- Free blocks are coalesced with adjacent neighbors when possible, and entire whole-page regions at the end of the heap are released back with `sbrk()` when safe.

## Files

- `mem_alloc.c` — allocator implementation.
- `mem_alloc.h` — public API header for the allocator.
- `test_mem_alloc.c` — validation and regression suite.

## Public interface

The allocator exposes the following API in `mem_alloc.h`:

```c
void *an_malloc(ssize_t size);
bool  an_free(void *ptr);
void *an_realloc(void *ptr, ssize_t size);
void *an_calloc(size_t nmemb, size_t size);
```

Notes:

- `an_malloc(size)` allocates `size` bytes and returns a pointer, or `NULL` on failure.
- `an_free(ptr)` frees an allocation returned by this allocator.
- `an_realloc(ptr, size)` follows glibc-style semantics and may resize in place or move the allocation.
- `an_calloc(nmemb, size)` allocates and zero-initializes memory with overflow checking.

## Build and test

From the repository root:

```sh
mkdir -p bin
cc -g -O0 -Wall -o bin/test_mem_alloc test_mem_alloc.c
./bin/test_mem_alloc
```

Useful test options:

```sh
./bin/test_mem_alloc -l           # list available tests
./bin/test_mem_alloc coalesce     # run tests whose names contain "coalesce"
```

The test suite includes invariant checks after each operation and runs each case in a forked child process so failures are isolated and reported cleanly.

## Requirements

This project expects a POSIX-like environment with:

- `sbrk()` support
- `fork()` / `waitpid()`
- `setrlimit()` for the OOM tests

The project is intended for Linux-like systems and is not portable to environments that lack a process heap break API.

## Important limitations

- This allocator changes the process break with `sbrk()`. Do not mix it with libc allocation functions in the same process.
- It is a single-threaded allocator and is not meant to be a full replacement for a production memory allocator.
- The implementation is intentionally minimal and educational, with explicit heap invariants and test coverage designed to validate behavior.
