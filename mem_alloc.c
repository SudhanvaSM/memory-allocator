#include "mem_alloc.h"
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ALIGNMENT _Alignof(max_align_t)
#define PAGE_SIZE 4096
#define INIT_MARKER 0x55
#define BLOCK_MARKER 0xDD
#define MIN_PAYLOAD ((size_t)16)

// Global header stats
typedef struct allocation_stats {
	uint8_t init_marker;
	bool lock;
	size_t blocks;
	size_t pages;
} my_stats;

// Per block header
typedef struct free_area {
	uint8_t marker;
	struct free_area *prev;
	struct free_area *next;
	bool in_use;
	size_t length;
} area;

static char *heap_start = NULL;
static bool allocator_initialized = false;

// align all sizes to 16 Byte widths
static size_t align_up(size_t size) {
	if (size > SIZE_MAX - (ALIGNMENT - 1)) return 0;
	return (size + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);
}

// align page size to 4096 increments
static size_t align_page_up(size_t size) {
	if (size > SIZE_MAX - (PAGE_SIZE - 1)) return 0;
	return (size + (PAGE_SIZE - 1)) & ~(size_t)(PAGE_SIZE - 1);
}

// helper functions
static inline size_t header_size(void) {
	return align_up(sizeof(area));
} 

static inline size_t stats_size(void) {
	return align_up(sizeof(my_stats));
}

static void fatal(const char *msg, size_t len) {
	ssize_t r = write(STDERR_FILENO, msg, len);
	(void)r;
	abort();
}

static inline void *payload_of(area *block) {
	return ((char *)block + header_size());
}

#define FATAL(literal) fatal(literal, sizeof(literal) - 1)

static my_stats *get_malloc_header(void) {
	if (!allocator_initialized) 
		FATAL("FATAL: allocator not initialized.\n");
	if (!heap_start) 
		FATAL("Fatal: uninitialized heap start.\n");
    my_stats *malloc_header = (my_stats *)heap_start;
    if (malloc_header->init_marker != INIT_MARKER) 
		FATAL("Fatal: corrupted allocator header.\n");
    return malloc_header;
}

static void *sbrk_return_current_break(void) {
	void *heap_location = sbrk(0);
	if (heap_location == (void *) -1) {
		perror("sbrk(0) failed.\n");
		return NULL;
	}
	return heap_location;
}

static int sbrk_step(ssize_t size) {
	if (size == 0) return 0;
	void *prev_break = sbrk(size);
	if (prev_break == (void *) -1) {
		perror("sbrk change size failed.\n");
		return 1;
	}
	return 0;
}

static area *find_last_block(void) {
	my_stats *malloc_header = get_malloc_header();
	area *block = (area *)((char *)malloc_header + stats_size());
	while (block->next) block = block->next;
	return block;
}

static area *find_best_fit(size_t aligned_size) {
	area *best = NULL;
	area *block = (area *)(heap_start + stats_size());
	while(block) {
		if (block->marker != BLOCK_MARKER) 
			FATAL("FATAL: heap corruption (bad block marker).\n");
		if (block->length >= aligned_size && !block->in_use) {
			if (!best || best->length > block->length) 
				best = block;
		}
		block = block->next;
	}
	return best;
}

static area *find_block_from_payload(void *ptr) {
	my_stats *malloc_header = get_malloc_header();
	area *temp = (area *)((char *)malloc_header + stats_size());
	while (temp) {
		if (payload_of(temp) == ptr) return temp;
		temp = temp->next;
	}
	return NULL;
}

// Combine current block with next block if its free
static void absorb_next_free_block(area *block, my_stats* malloc_header) {
	area *next_free_block = block->next;
	block->length += header_size() + next_free_block->length;
	block->next = next_free_block->next;
	if (next_free_block->next) next_free_block->next->prev = block;
	malloc_header->blocks -= 1;
}

// If a block has excess payload length than required, we split it into
// two blocks iff after the split, the new block has a minimum length
// that can handle accommodate the header and MINIMUM_PAYLOAD
static void split_block(area *block, size_t aligned_size, my_stats *malloc_header) {
	size_t leftover_size = block->length - aligned_size;
	if (leftover_size >= header_size() + MIN_PAYLOAD) {
		area *new_block = (area *)((char *)block + header_size() + aligned_size);
		new_block->in_use = false;
		new_block->length = leftover_size - header_size();
		new_block->marker = BLOCK_MARKER;
		new_block->next = block->next;
		new_block->prev = block;
		if (new_block->next) new_block->next->prev = new_block;
		block->next = new_block;
		block->length = aligned_size;
		malloc_header->blocks += 1;
		// if next block is unused, combine both blocks
		if (new_block->next && !new_block->next->in_use) absorb_next_free_block(new_block, malloc_header);
	}
}

// Grow last block by required_size amount of bytes
static bool grow_tail(area* last_block, size_t required_size, my_stats *malloc_header) {
	if (last_block->length >= required_size) return true;
	size_t deficit = required_size - last_block->length;
	size_t bytes_to_add = align_page_up(deficit); // find exact number of bytes that is a multiple of 4096
	if (bytes_to_add == 0 || 
		bytes_to_add > (size_t)SSIZE_MAX || 
		last_block->length > SIZE_MAX - bytes_to_add ||
		sbrk_step(bytes_to_add) != 0) {
		errno = ENOMEM;
		return false;
	}
	last_block->length += bytes_to_add;
	malloc_header->pages += bytes_to_add / PAGE_SIZE;
	return true;
}

// Using integer division with multiplication
// it's guaranteed that bytes_to_reduce is always
// multiple of PAGE_SIZE, keeping the pages aligned
static void reduce_heap_size_if_possible(void) {
	my_stats *malloc_header = get_malloc_header();
	area *last_block = find_last_block();
	if (last_block->in_use) return;
	size_t pages_to_reduce = last_block->length / PAGE_SIZE;
	size_t bytes_to_reduce = pages_to_reduce * PAGE_SIZE;
	if (sbrk_step(-(ssize_t)bytes_to_reduce) != 0) {
		errno = ENOMEM;
		return;
	}
	last_block->length -= bytes_to_reduce;
	malloc_header->pages -= pages_to_reduce;
}

// Uses a boolean flag allocator_initialized to make sure
// heap is not initialized again
static bool initialize_heap(void) {
	char *start = sbrk_return_current_break();
	if (!start || sbrk_step(PAGE_SIZE) != 0) return false;
	heap_start = start;
	my_stats *malloc_header = (my_stats *)heap_start;
	malloc_header->init_marker = INIT_MARKER;
	malloc_header->lock = false;
	malloc_header->blocks = malloc_header->pages = 1;
	area *first_block = (area *)((char *)malloc_header + stats_size());
	first_block->in_use = false;
	first_block->length = PAGE_SIZE - stats_size() - header_size();
	first_block->marker = BLOCK_MARKER;
	first_block->prev = first_block->next = NULL;
	allocator_initialized = true;
	return true;
}

// finds best block using best fit strategy
static void *add_block(size_t size) {
	my_stats *malloc_header = get_malloc_header();
	while(malloc_header->lock) sleep(1);
	size_t aligned_size = align_up(size);
	if (aligned_size == 0) {
		errno = ENOMEM;
		return NULL;
	}
	malloc_header->lock = true;
	area *best_block = find_best_fit(aligned_size);
	// if no block is found, we need to grow the last block
	if (!best_block) best_block = find_last_block();
	if (!best_block->next) {
		// last block needs enough space for remainder
		size_t overhead = header_size() + MIN_PAYLOAD;
		size_t required_size = aligned_size + overhead;
		if (aligned_size > SIZE_MAX - overhead || !grow_tail(best_block, required_size, malloc_header)) {
			malloc_header->lock = false;
			errno = ENOMEM;
			return NULL;
		}
	}
	best_block->in_use = true;
	split_block(best_block, aligned_size, malloc_header);
	malloc_header->lock = false;
	return payload_of(best_block);
}

void *an_malloc(ssize_t size) {
	if (size <= 0) return NULL;
	if (!allocator_initialized && !initialize_heap()) {
		FATAL("FATAL: heap not initialized.\n");
	}
	return add_block((size_t)size);
}

bool an_free(void *ptr) {
	if (!ptr) return false;
	my_stats *malloc_header = get_malloc_header();
	while (malloc_header->lock) sleep(1);
	malloc_header->lock = true;
	area *block = find_block_from_payload(ptr);
	if (!block || block->marker != BLOCK_MARKER || !block->in_use) {
		malloc_header->lock = false;
		return false;
	}
	block->in_use = false;
	// block next to current block is free and hence combine with next block
	if (block->next && !block->next->in_use)
		absorb_next_free_block(block, malloc_header);
	// block previous to current block is free and hence combine with previous block
	// we do this by making the previous block the current block
	if (block->prev && !block->prev->in_use) {
		block = block->prev;
		absorb_next_free_block(block, malloc_header);
	}
	reduce_heap_size_if_possible();
	malloc_header->lock = false;
	return true;
}

void *an_realloc(void *ptr, ssize_t size) {
	if (!ptr) return an_malloc(size);
	if (size == 0) {
		an_free(ptr);
		return NULL;
	}
	if (size < 0) {
		errno = EINVAL;
		return NULL;
	}
	my_stats *malloc_header = get_malloc_header();
	while (malloc_header->lock) sleep(1);
	malloc_header->lock = true;
	area *block = find_block_from_payload(ptr);
	if (!block || block->marker != BLOCK_MARKER || !block->in_use) {
		malloc_header->lock = false;
		errno = EINVAL;
		return false;
	}
	size_t aligned_size = align_up((size_t)size);
	if (aligned_size == 0) {
		malloc_header->lock = false;
		errno = ENOMEM;
		return false;
	}
	// if same size requested, do nothing and return same pointer
	if (aligned_size == block->length) {
		malloc_header->lock = false;
		return ptr;
	}
	// shrink in-place by splitting the block 
	if (aligned_size < block->length) {
		split_block(block, aligned_size, malloc_header);
		reduce_heap_size_if_possible();
		malloc_header->lock = false;
		return ptr;
	}
	// grow in-place by abosrbing free neighbour
	area *next_block = block->next;
	if (next_block && !next_block->in_use) {
		size_t total = block->length + header_size() + next_block->length;
		size_t need = 0;
		// we only need space for aligned_size, no need for header
		if (next_block->next) need = aligned_size;
		// last block, need to account for header and MIN_PAYLOAD
		else need = aligned_size + header_size() + MIN_PAYLOAD;
		if (total >= need) {
			absorb_next_free_block(block, malloc_header);
			split_block(block, aligned_size, malloc_header);
			malloc_header->lock = false;
			return ptr;
		}
	}
	// lock acquired by an_malloc and an_free
	malloc_header->lock = false;
	// no space, create new block. 
	// original block untouched
	void *fresh_new_block = an_malloc(size);
	if (!fresh_new_block) return NULL;
	memcpy(fresh_new_block, ptr, block->length);
	an_free(ptr);
	return fresh_new_block;
}

void *an_calloc(size_t nmemb, size_t size) {
	// overflow check
	if (nmemb != 0 && size > SIZE_MAX / nmemb) {
		errno = ENOMEM;
		return NULL;
	}
	size_t total = size * nmemb;
	if (total > SSIZE_MAX) {
		errno = ENOMEM;
		return NULL;
	}
	void *ptr = an_malloc((ssize_t)total);
	if (ptr) memset(ptr, 0, total);
	return ptr;
}

// Debug helper: print the block list starting at `head`. 
void display_ll(area *head) {
	printf("Linked List: ");
	for (; head; head = head->next)
		printf("%zu (%s) -> ", head->length, head->in_use ? "USED" : "FREE");
	printf("NULL\n");
}