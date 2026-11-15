#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <stdbool.h>
#include <limits.h>

#define ALIGNMENT _Alignof(max_align_t)
#define PAGE_SIZE 4096
#define INIT_MARKER 0x55
#define BLOCK_MARKER 0xDD
#define MIN_PAYLOAD ((size_t)16)

typedef struct free_area {
	uint8_t marker;
	struct free_area *prev;
	struct free_area *next;
	bool in_use;
	size_t length;
} area;

typedef struct allocation_stats {
	uint8_t init_marker;
	bool lock;
	size_t blocks;
	size_t pages;
} my_stats;

static char *heap_start = NULL;
static bool allocator_initialized = false;

static size_t align_up(size_t size) {
	if (size > SIZE_MAX - (ALIGNMENT - 1)) return 0;
	return (size + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);
}

static size_t align_page_up(size_t size) {
	if (size > SIZE_MAX - (PAGE_SIZE - 1)) return 0;
	return (size + (PAGE_SIZE - 1)) & ~(size_t)(PAGE_SIZE - 1);
}

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
			if (!best || best->length >= block->length) 
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

static void absorb_next_free_block(area *block, my_stats* malloc_header) {
	area *next_free_block = block->next;
	block->length += header_size() + next_free_block->length;
	block->next = next_free_block->next;
	if (next_free_block->next) next_free_block->next->prev = block;
	malloc_header->blocks -= 1;
}

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
		if (new_block->next && !new_block->next->in_use) absorb_next_free_block(new_block, malloc_header);
	}
}

static bool grow_tail(area* last_block, size_t required_size, my_stats *malloc_header) {
	if (last_block->length >= required_size) return true;
	size_t deficit = required_size - last_block->length;
	size_t bytes_to_add = align_page_up(deficit);
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
	if (!best_block) best_block = find_last_block();
	if (!best_block->next) {
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
	if (block->next && !block->next->in_use)
		absorb_next_free_block(block, malloc_header);
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
	if (aligned_size == block->length) {
		malloc_header->lock = false;
		return ptr;
	}
	if (aligned_size < block->length) {
		split_block(block, aligned_size, malloc_header);
		reduce_heap_size_if_possible();
		malloc_header->lock = false;
		return ptr;
	}
	area *next_block = block->next;
	if (next_block && !next_block->in_use) {
		size_t total = block->length + header_size() + next_block->length;
		size_t need = 0;
		if (next_block->next) need = aligned_size;
		else need = aligned_size + header_size() + MIN_PAYLOAD;
		if (total >= need) {
			absorb_next_free_block(block, malloc_header);
			split_block(block, aligned_size, malloc_header);
			malloc_header->lock = false;
			return ptr;
		}
	}
	malloc_header->lock = false;
	void *fresh_new_block = an_malloc(size);
	if (!fresh_new_block) return NULL;
	memcpy(fresh_new_block, ptr, block->length);
	an_free(ptr);
	return fresh_new_block;
}

void *an_calloc(size_t nmemb, size_t size) {
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

// void display_ll(area *head) {
//     printf("Linked List: ");

//     while (head) {
//         printf("%zu (%s) -> ", head->length, head->in_use ? "USED" : "FREE");
//         head = head->next;
//     }

//     printf("NULL\n");
// }

// void test_linked_list(area *head) {
//     area *previous = NULL;

//     while (head != NULL) {
//         assert(head->marker == BLOCK_MARKER);
//         assert(head->prev == previous);

//         if (head->next != NULL) {
//             assert(head->next->prev == head);
//         }

//         previous = head;
//         head = head->next;
//     }

//     assert(previous != NULL);
//     assert(previous->in_use == false);

//     printf("Linked list integrity: PASSED\n");
// }

// void test_basic_malloc() {
//     char *ptr = (char *)an_malloc(8192);
// 	char *ptr1 = (char *)an_malloc(16384);
// 	char *ptr2 = (char *)an_malloc(32768);

//     assert(ptr != NULL);
// 	assert(ptr1 != NULL);
// 	assert(ptr2 != NULL);
// 	assert(ptr != ptr1);
// 	assert(ptr != ptr2);
// 	assert(ptr1 != ptr2);

//     area *first_block = (area *)((char *)ptr - align_up(sizeof(area)));

//     assert(first_block->marker == BLOCK_MARKER);
//     assert(first_block->in_use == true);

// 	memset(ptr, 'A', 8192);
// 	memset(ptr1, 'B', 16384);
// 	memset(ptr2, 'C', 32768);

//     for (size_t i = 0; i < 8192; i++)
//    		assert(ptr[i] == 'A');

// 	for (size_t i = 0; i < 16384; i++)
// 		assert(ptr1[i] == 'B');

// 	for (size_t i = 0; i < 32768; i++)
// 		assert(ptr2[i] == 'C');

// 	my_stats *malloc_header = get_malloc_header();

// 	printf("Basic Malloc result %p\n", (void *)ptr);
// 	printf("Block size: %zu\n", first_block->length);
// 	printf("Number of Blocks: %zu\n", malloc_header->blocks);
// 	printf("Number of Pages: %zu\n", malloc_header->pages);

// 	display_ll(first_block);
// 	test_linked_list(first_block);
// }

// void test_free() {

// 	int arr[100] = {
// 		3444, 346, 2017, 2506, 2526, 3538, 1802, 3453, 1508, 1406,
// 		647, 3270, 3564, 286, 3415, 3322, 1105, 108, 808, 1253,
// 		1386, 3222, 408, 3920, 2407, 2486, 1626, 738, 2690, 1651,
// 		206, 3017, 3368, 3620, 3397, 455, 3380, 349, 1001, 2713,
// 		2548, 3470, 1582, 458, 2882, 822, 1408, 712, 240, 2137,
// 		3880, 3611, 4011, 1159, 4079, 3026, 1640, 1136, 3485, 3778,
// 		7, 1105, 3461, 3790, 1169, 2846, 57, 319, 2572, 712,
// 		2193, 3062, 2420, 897, 3088, 3336, 2376, 1017, 2764, 1953,
// 		1960, 175, 2168, 1950, 1474, 417, 2901, 2416, 2075, 2809,
// 		3837, 2748, 3077, 3682, 631, 1455, 606, 3017, 583, 450
// 	};

// 	char *ptr[150] = {NULL};

// 	for (int i = 0; i < 100; i++) {
// 		ptr[i] = (char *)an_malloc((size_t)arr[i]);

// 		if (ptr[i] == NULL) {
// 			printf("Allocation failed at index %d, size %d\n", i, arr[i]);
// 			break;
// 		}
// 	}

// 	area *first_block = (area *)((char *)ptr[0] - align_up(sizeof(area)));
// 	assert(first_block->next != NULL);

// 	area *temp = first_block;
// 	while (temp) {
// 		assert(temp->marker == BLOCK_MARKER);

// 		if (temp->next != NULL) {
// 			assert(temp->in_use == true);
// 		} else {
// 			assert(temp->in_use == false);
// 		}

// 		temp = temp->next;
// 	}

// 	printf("After allocation: ");
// 	display_ll(first_block);

// 	my_stats *malloc_header = get_malloc_header();
// 	printf("Number of blocks: %zu\n", malloc_header->blocks);
// 	printf("Number of pages: %zu\n", malloc_header->pages);

// 	printf("\nFreeing every alternate block.\n");
// 	for (int i = 0; i < 100; i += 2) {
// 		an_free(ptr[i]);
// 	}
// 	printf("After freeing: ");
// 	display_ll(first_block);
// 	printf("Number of blocks: %zu\n", malloc_header->blocks);
// 	printf("Number of pages: %zu\n", malloc_header->pages);

// 	int arr2[50] = {
// 		1468, 2116, 1549, 2351, 3241, 686, 1335, 3969, 3509, 2215,
// 		3895, 2812, 4083, 980, 1111, 28, 14, 2211, 3614, 2456,
// 		619, 2124, 3789, 296, 3705, 3563, 689, 149, 185, 3741,
// 		2070, 3281, 2086, 4057, 2497, 2276, 1128, 2535, 1438, 2189,
// 		2045, 3115, 3755, 2325, 1006, 3886, 502, 593, 1469, 3910
// 	};

// 	for (int i = 0; i < 50; i++) {
// 		ptr[100 + i] = (char *)an_malloc((size_t)arr2[i]);
// 	}

// 	printf("After 50 more blocks: ");
// 	display_ll(first_block);
// 	printf("Number of blocks: %zu\n", malloc_header->blocks);
// 	printf("Number of pages: %zu\n", malloc_header->pages);

// 	printf("Free every block.\n");
// 	for (int i = 0; i < 150; i++) {
// 		an_free(ptr[i]);
// 	}
// 	display_ll(first_block);
// 	printf("Number of blocks: %zu\n", malloc_header->blocks);
// 	printf("Number of pages: %zu\n", malloc_header->pages);
// }

// void test_reduce_heap_size(void) {
// 	char *ptr = (char *)an_malloc(128);
// 	char *ptr1 = (char *)an_malloc(64);
// 	char *ptr2 = (char *)an_malloc(256);
// 	char *ptr3 = (char *)an_malloc(1024);

// 	char *ptr4 = (char *)an_malloc(12228);

// 	area *first_block = (area *)((char *)ptr - align_up(sizeof(area)));

// 	printf("After allocation: ");
// 	display_ll(first_block);
// 	my_stats *malloc_header = get_malloc_header();
// 	printf("Number of blocks: %zu\n", malloc_header->blocks);
// 	printf("Number of pages: %zu\n", malloc_header->pages);

// 	void *heap_before_free = sbrk_return_current_break();
// 	an_free(ptr4);
// 	void *heap_after_free = sbrk_return_current_break();
// 	assert((char *)heap_before_free - (char *)heap_after_free == 3 * PAGE_SIZE);

// 	printf("After freeing: ");
// 	display_ll(first_block);
// 	printf("Number of blocks: %zu\n", malloc_header->blocks);
// 	printf("Number of pages: %zu\n", malloc_header->pages);

// 	ptr4 = (char *)an_malloc(4096);

// 	printf("After allocating again: ");
// 	display_ll(first_block);
// 	printf("Number of blocks: %zu\n", malloc_header->blocks);
// 	printf("Number of pages: %zu\n", malloc_header->pages);
// }

// void test_calloc(void) {
// 	char *ptr = (char *)an_malloc(70);
// 	area *first_block = (area *)((char *)ptr - align_up(sizeof(area)));

//     assert(first_block->marker == BLOCK_MARKER);
//     assert(first_block->in_use == true);

// 	memset(ptr, 'A', 70);
// 	for (size_t i = 0; i < 70; i++) {
// 		assert(ptr[i] == 'A');
// 	}
// 	display_ll(first_block);
// 	an_free(ptr);
// 	display_ll(first_block);
// 	ptr = (char *)an_calloc(7, 10);
// 	for (size_t i = 0; i < 70; i++) {
// 		assert(ptr[i] == 0);
// 	}
// }

// void call_test(void (*test_func)(), const char *msg) {
// 	pid_t pid = fork();
// 	if (!pid) {
// 		test_func();
// 		exit(0);
// 	}
// 	int status;
// 	waitpid(pid, &status, 0);
// 	if (WIFSIGNALED(status))
// 		printf("%s crashed with signal %d\n", msg, WTERMSIG(status));
// 	else
// 		printf("%s passed.\n", msg);
// }

// int main() {
// 	printf("Running my Memory Allocator.\n");
// 	//call_test(test_basic_malloc, "Basic Malloc");
// 	//call_test(test_free, "Basic Free");
// 	//call_test(test_reduce_heap_size, "Basic Heap Reduction");
// 	call_test(test_calloc, "Basic calloc");
// 	return 0;
// }