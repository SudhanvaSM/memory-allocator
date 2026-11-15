# Memory Allocator

A small educational memory allocator written in C. It manages its own heap using the POSIX `sbrk()` interface and provides allocation routines similar to `malloc`, `free`, `realloc`, and `calloc`.

> **Status:** This is a learning project, not a replacement for the C standard library allocator. Because it uses the process-wide program break, do not mix it with other code that uses `sbrk()` or depend on it for production or multithreaded applications.

## Features

- Allocates memory in 4 KiB pages and grows the heap as needed.
- Aligns payloads to `_Alignof(max_align_t)`.
- Uses a doubly linked list of heap blocks and a best-fit placement policy.
- Splits reusable blocks and coalesces adjacent free blocks.
- Returns whole unused pages from the end of the heap where possible.
- Provides `an_malloc`, `an_free`, `an_realloc`, and `an_calloc` routines.
- Includes a test harness that checks heap invariants, allocation behavior, and stress cases.

## Files

- `mem_alloc.c` — allocator implementation.
- `test_mem_alloc.c` — test harness. It includes the allocator source so it can inspect internal heap state.
- `temp.c` — an additional C source file in the repository; the documented test harness uses `mem_alloc.c` by default.

## Requirements

A C compiler and a POSIX-like environment that supports `sbrk()`, `fork()`, and `setrlimit()` (for the out-of-memory test). The implementation and tests are intended for systems such as Linux; `sbrk()` is obsolete or unavailable on some platforms.

## Build and run the tests

From the repository root:

```sh
mkdir -p bin
cc -g -O0 -Wall -o bin/test_mem_alloc test_mem_alloc.c
./bin/test_mem_alloc
```

The harness runs each test in a child process so a failure or timeout is reported without stopping the rest of the suite. Some environments may skip the out-of-memory test if they do not enforce the required data limit.

Useful test options:

```sh
./bin/test_mem_alloc -l           # list test names
./bin/test_mem_alloc coalesce     # run tests whose names contain "coalesce"
```

To test another allocator source file, pass its name as a quoted macro value at compile time:

```sh
cc -g -O0 -Wall -DALLOCATOR_SRC='"other.c"' \
  -o bin/test_mem_alloc test_mem_alloc.c
```

## Allocator API

The implementation provides these functions in `mem_alloc.c`:

```c
void *an_malloc(ssize_t size);
bool an_free(void *ptr);
void *an_realloc(void *ptr, ssize_t size);
void *an_calloc(size_t nmemb, size_t size);
```

Include the appropriate standard headers (`<stdbool.h>`, `<stddef.h>`, and `<sys/types.h>`) when declaring these functions in a program. This repository does not currently provide a separate public header file.

- `an_malloc(size)` allocates `size` bytes and returns a pointer, or `NULL` if the request cannot be satisfied. Non-positive sizes return `NULL`.
- `an_free(ptr)` releases a pointer allocated by this allocator. It returns `true` when the allocation is freed and `false` for `NULL`, invalid, or already-freed pointers.
- `an_realloc(ptr, size)` resizes an allocation. It may resize in place or move the allocation; when it moves, the existing contents are copied up to the smaller of the old and new sizes. A `NULL` pointer behaves like `an_malloc`; a size of zero frees the allocation and returns `NULL`.
- `an_calloc(nmemb, size)` allocates space for an array and zero-initializes it. It checks for multiplication overflow.

## Important limitations

- This allocator obtains memory by changing the process break with `sbrk()`. Do not use it alongside the system allocator or other components that may also change the program break.
- It is a standalone project implementation, not a drop-in implementation of the standard `malloc` family. It does not provide the standard names or a public header.
- The allocator's internal lock is not a substitute for a production-grade thread-synchronization design. Do not assume the implementation is thread-safe.
- The test harness itself warns against calling libc allocation functions after the allocator has initialized, since they may move the program break.
