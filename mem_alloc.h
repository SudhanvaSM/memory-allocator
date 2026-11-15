#ifndef MEM_ALLOC_H
#define MEM_ALLOC_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Single-threaded allocator built on sbrk. Do not mix with libc malloc in the same process. */

void *an_malloc(ssize_t size);               /* NULL for size <= 0 or on failure (errno = ENOMEM) */
bool  an_free(void *ptr);                    /* false if ptr is NULL, foreign, or already freed   */
void *an_realloc(void *ptr, ssize_t size);   /* glibc-style semantics, see README                 */
void *an_calloc(size_t nmemb, size_t size);  /* zeroed; overflow-checked                          */

#ifdef __cplusplus
}
#endif

#endif /* MEM_ALLOC_H */