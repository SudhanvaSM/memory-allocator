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

#define ALIGNMENT _Alignof(max_align_t)

const int INIT_MARKER = 0x55;
const int BLOCK_MARKER = 0xDD;
const int PAGE_SIZE = 4096;
const size_t MIN_PAYLOAD = 16;

size_t align_up(size_t size) {
	if (size > SIZE_MAX - (ALIGNMENT - 1)) return 0;
    return (size + (ALIGNMENT - 1)) & ~(ALIGNMENT - 1);
}

size_t align_page_up(size_t size) {
	if (size > SIZE_MAX - (PAGE_SIZE - 1)) return 0;
	return (size + (PAGE_SIZE - 1)) & ~(size_t)(PAGE_SIZE - 1);
}

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

const int FIRST_BLOCK_OFFSET = sizeof(area);
char *heap_start = NULL;

bool allocator_initialized = false;

static my_stats *get_malloc_header(void) {
	if (!allocator_initialized) {
		const char msg[] = "Fatal: allocator not initialized.\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
        abort();
	}
    if (!heap_start) {
        const char msg[] = "Fatal: uninitialized heap start.\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
        abort();
    }

    my_stats *malloc_header = (my_stats *)heap_start;
    if (malloc_header->init_marker != INIT_MARKER) {
        const char msg[] = "Fatal: corrupted allocator header.\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
        abort();
    }

    return malloc_header;
}

area *find_last_block(void) {
	my_stats *malloc_header = get_malloc_header();
	area *block = (area *)((char *)malloc_header + align_up(sizeof(my_stats)));
	while (block->next) block = block->next;
	return block;
}

area *find_previous_used_block(area *ptr) {
	area *mov_ptr = ptr;
	while (mov_ptr->prev) {
		mov_ptr = mov_ptr->prev;
		if (mov_ptr->in_use) return mov_ptr;
	}
	return NULL;
}

void* sbrk_return_current_break(void) {
	void* heap_location = sbrk(0);
	if (heap_location == (void *) -1) {
		perror("sbrk(0) failed.\n");
		return NULL;
	}
	return heap_location;
}

int sbrk_step(ssize_t size) {
	if (size == 0) return 0;
	void *prev_break = sbrk(size);
	if (prev_break == (void *) -1) {
		perror("sbrk change size failed.\n");
		return 1;
	}
	return 0;
}

void *add_new_trailing_block(size_t remaining_size) {
	area* last_block = find_last_block();
	area *new_block = (area *)((char *)last_block + last_block->length + align_up(sizeof(area)));
	new_block->in_use = false;
	new_block->length = remaining_size;
	new_block->marker = BLOCK_MARKER;
	new_block->next = NULL;
	new_block->prev = last_block;
	last_block->next = new_block;
	return new_block;
}

void *add_used_block_best_fit(size_t size) {
	my_stats *malloc_header = get_malloc_header();
	while (malloc_header->lock) sleep(1);

	size_t aligned_size = align_up(size);
	if (aligned_size == 0) return NULL;

	malloc_header->lock = true;
	area *block = (area *)(heap_start + align_up(sizeof(my_stats)));
	area *smallest_block = NULL;
	area *last_block = block;

	while (block != NULL) {
		assert(block->marker == BLOCK_MARKER);
		if (block->length >= aligned_size && !block->in_use) {
			if (!smallest_block || smallest_block->length > block->length) {
				smallest_block = block;
			}
		}
		block = block->next;
	}
	if (!smallest_block) {
		last_block = find_last_block();

		const size_t overhead = align_up(sizeof(area)) + MIN_PAYLOAD;
		if (aligned_size > SIZE_MAX - overhead) {
			// Overflow would occur: aligned_size is too large to ever split or allocate
			errno = ENOMEM;
			malloc_header->lock = false;
			return NULL;
		}

		size_t required_size = aligned_size + overhead;
		assert(!last_block->in_use);
		while (last_block->length < required_size) {
			size_t deficit = required_size - last_block->length;
			if (deficit > SIZE_MAX - (size_t)(PAGE_SIZE - 1)) {
				malloc_header->lock = false;
				errno = ENOMEM;
				return NULL;
			}
			size_t bytes_to_add = align_page_up(deficit);
			if (last_block->length > SIZE_MAX - bytes_to_add) {
				malloc_header->lock = false;
				errno = ENOMEM;
				return NULL;
			}
			if (sbrk_step(bytes_to_add) != 0) {
				malloc_header->lock = false;
				errno = ENOMEM;
				return NULL;
			}
			last_block->length += bytes_to_add;
			malloc_header->pages += bytes_to_add / PAGE_SIZE;
		}
		size_t last_block_size = last_block->length;
		last_block->in_use = true;
		last_block->length = aligned_size;
		last_block->marker = BLOCK_MARKER;
		last_block->next = NULL;

		size_t remaining_size = last_block_size - aligned_size - align_up(sizeof(area));
		add_new_trailing_block(remaining_size);
		malloc_header->blocks += 1;
		smallest_block = last_block;
	}
	else {
		size_t min_split_overhead = align_up(sizeof(area)) + MIN_PAYLOAD;
		if (smallest_block->length >= min_split_overhead 
			&& smallest_block->length - min_split_overhead >= aligned_size) {

			size_t old_length = smallest_block -> length;
			area *new_block = (area *)((char *)smallest_block + align_up(sizeof(area)) + aligned_size);
			new_block->in_use = false;
			new_block->marker = BLOCK_MARKER;
			new_block->prev = smallest_block;
			new_block->next = smallest_block->next;
			if (new_block->next) (new_block->next)->prev = new_block;
			smallest_block->next = new_block;
			new_block->length = old_length - aligned_size - align_up(sizeof(area));
			smallest_block->length = aligned_size;
			smallest_block->in_use = true;
			malloc_header->blocks += 1;
		}
		else if (smallest_block->next) {
			smallest_block->in_use = true;
		}
		else {
			const size_t overhead = align_up(sizeof(area)) + MIN_PAYLOAD;
			if (aligned_size > SIZE_MAX - overhead) {
				// Overflow would occur: aligned_size is too large to ever split or allocate
				errno = ENOMEM;
				malloc_header->lock = false;
				return NULL;
			}

			size_t required_size = aligned_size + overhead;
			size_t deficit = required_size - smallest_block->length;
			if (deficit > SIZE_MAX - (size_t)(PAGE_SIZE - 1)) {
				malloc_header->lock = false;
				errno = ENOMEM;
				return NULL;
			}
			size_t bytes_to_add = align_page_up(deficit);
			if (smallest_block->length > SIZE_MAX - bytes_to_add) {
				malloc_header->lock = false;
				errno = ENOMEM;
				return NULL;
			}
			if (sbrk_step(bytes_to_add) != 0) {
				malloc_header->lock = false;
				errno = ENOMEM;
				return NULL;
			}
			smallest_block->length += bytes_to_add;
			malloc_header->pages += bytes_to_add / PAGE_SIZE;
			size_t old_length = smallest_block->length;
			smallest_block->length = aligned_size;
			smallest_block->in_use = true;
			size_t remaining_size = old_length - smallest_block->length - align_up(sizeof(area));
			add_new_trailing_block(remaining_size);
			malloc_header->blocks += 1;
		}
	}
	malloc_header->lock = false;
	return (void *)((char *) smallest_block + align_up(sizeof(area)));
}

void reduce_heap_size_if_possible(void) {
	area *last_block = find_last_block();
	if (last_block->in_use) return;

	area* prev_used_block = find_previous_used_block(last_block);
	my_stats *malloc_header = get_malloc_header();

	if (!prev_used_block) {
		size_t required_header_size = align_up(sizeof(my_stats)) + align_up(sizeof(area));
		size_t required_payload_size = PAGE_SIZE - required_header_size;

		size_t bytes_to_reduce = (last_block->length + align_up(sizeof(area)) + align_up(sizeof(my_stats))) - PAGE_SIZE;
		if (sbrk_step(-(ssize_t)bytes_to_reduce) != 0) {
			errno = ENOMEM;
			return;
		}
		last_block -> length = required_payload_size;
		malloc_header->blocks = 1;
		malloc_header->pages = 1;
		return;
	}
	else {
		size_t reducible = last_block->length;
		size_t bytes_to_reduce = (reducible / PAGE_SIZE) * PAGE_SIZE;
		
		if (sbrk_step((ssize_t)-bytes_to_reduce) != 0) {
			errno = ENOMEM;
			return;
		}

		last_block->length -= bytes_to_reduce;
		size_t pages_released = bytes_to_reduce / PAGE_SIZE;
		malloc_header->pages -= pages_released;
		return;
	}
}

void *an_malloc(ssize_t size) {
	if (size <= 0) return NULL;
	if (heap_start == NULL) {
		heap_start = sbrk_return_current_break();
		if (heap_start == NULL || sbrk_step(4096) != 0) {
			heap_start = NULL;
			return NULL;
		}
	}
	char *heap_end = sbrk_return_current_break();
	if (heap_end == NULL) {
		heap_start = NULL;
		return NULL;
	}
	long int length = heap_end - heap_start;
	if (!allocator_initialized) {
		my_stats *malloc_header = (my_stats *)heap_start;
		malloc_header->init_marker = INIT_MARKER;
		malloc_header->lock = false;
		malloc_header->blocks = malloc_header->pages = 1;

		area *first_block = (area *)(heap_start + align_up(sizeof(my_stats)));
		first_block->in_use = false;
		first_block->length = length - align_up(sizeof(my_stats)) - align_up(sizeof(area));
		first_block->marker = BLOCK_MARKER;
		first_block->prev = first_block->next = NULL;

		allocator_initialized = true;
	}
	return add_used_block_best_fit(size);
}

area *find_block_from_payload(void *ptr) {
	my_stats *malloc_header = get_malloc_header();
	area *temp = (area *)((char *)malloc_header + align_up(sizeof(my_stats)));
	while (temp) {
		void *payload = (char *)temp + align_up(sizeof(area));
		if (payload == ptr) return temp;
		temp = temp->next;
	}
	return NULL;
}

bool an_free(void *ptr) {
	if (!ptr) {
		return false;
	}
	my_stats *malloc_header = get_malloc_header();
	while(malloc_header->lock) sleep(1);
	malloc_header->lock = true;

	area *block = find_block_from_payload(ptr);
	if (!block || block->marker != BLOCK_MARKER) {
		malloc_header->lock = false;
		return false;
	}

	if (!block->in_use) {
		malloc_header->lock = false;
		return false;
	}
	block->in_use = false;
	if (block->next && !(block->next)->in_use) {
		area *not_used_next_block = block->next;
		if (not_used_next_block) {
			block->next = not_used_next_block->next;
			if (not_used_next_block->next) not_used_next_block->next->prev = block;
			else block->next = NULL;
		}
		block->length += align_up(sizeof(area)) + not_used_next_block->length;
		malloc_header->blocks -= 1;
	}
	if (block-> prev && !(block->prev)->in_use) {
		area *to_delete_block = block;
		block = block->prev;
		block->length += align_up(sizeof(area)) + to_delete_block->length;
		block->next = to_delete_block->next;
		if (block->next) block->next->prev = block;
		malloc_header->blocks -= 1;
	}
	reduce_heap_size_if_possible();
	malloc_header->lock = false;
	return true;
}

void display_ll(area *head) {
    printf("Linked List: ");

    while (head) {
        printf("%zu (%s) -> ", head->length, head->in_use ? "USED" : "FREE");
        head = head->next;
    }

    printf("NULL\n");
}

void test_linked_list(area *head) {
    area *previous = NULL;

    while (head != NULL) {
        assert(head->marker == BLOCK_MARKER);
        assert(head->prev == previous);

        if (head->next != NULL) {
            assert(head->next->prev == head);
        }

        previous = head;
        head = head->next;
    }

    assert(previous != NULL);
    assert(previous->in_use == false);

    printf("Linked list integrity: PASSED\n");
}

void test_basic_malloc() {
    char *ptr = (char *)an_malloc(8192);
	char *ptr1 = (char *)an_malloc(16384);
	char *ptr2 = (char *)an_malloc(32768);

    assert(ptr != NULL);
	assert(ptr1 != NULL);
	assert(ptr2 != NULL);
	assert(ptr != ptr1);
	assert(ptr != ptr2);
	assert(ptr1 != ptr2);

    area *first_block = (area *)((char *)ptr - align_up(sizeof(area)));

    assert(first_block->marker == BLOCK_MARKER);
    assert(first_block->in_use == true);

	memset(ptr, 'A', 8192);
	memset(ptr1, 'B', 16384);
	memset(ptr2, 'C', 32768);

    for (size_t i = 0; i < 8192; i++)
    assert(ptr[i] == 'A');

	for (size_t i = 0; i < 16384; i++)
		assert(ptr1[i] == 'B');

	for (size_t i = 0; i < 32768; i++)
		assert(ptr2[i] == 'C');

	my_stats *malloc_header = get_malloc_header();

	printf("Basic Malloc result %p\n", (void *)ptr);
	printf("Block size: %zu\n", first_block->length);
	printf("Number of Blocks: %zu\n", malloc_header->blocks);
	printf("Number of Pages: %zu\n", malloc_header->pages);

	display_ll(first_block);
	test_linked_list(first_block);
}

void test_free() {

	int arr[100] = {
		3444, 346, 2017, 2506, 2526, 3538, 1802, 3453, 1508, 1406,
		647, 3270, 3564, 286, 3415, 3322, 1105, 108, 808, 1253,
		1386, 3222, 408, 3920, 2407, 2486, 1626, 738, 2690, 1651,
		206, 3017, 3368, 3620, 3397, 455, 3380, 349, 1001, 2713,
		2548, 3470, 1582, 458, 2882, 822, 1408, 712, 240, 2137,
		3880, 3611, 4011, 1159, 4079, 3026, 1640, 1136, 3485, 3778,
		7, 1105, 3461, 3790, 1169, 2846, 57, 319, 2572, 712,
		2193, 3062, 2420, 897, 3088, 3336, 2376, 1017, 2764, 1953,
		1960, 175, 2168, 1950, 1474, 417, 2901, 2416, 2075, 2809,
		3837, 2748, 3077, 3682, 631, 1455, 606, 3017, 583, 450
	};

	char *ptr[150] = {NULL};

	for (int i = 0; i < 100; i++) {
		ptr[i] = (char *)an_malloc((size_t)arr[i]);

		if (ptr[i] == NULL) {
			printf("Allocation failed at index %d, size %d\n", i, arr[i]);
			break;
		}
	}

	area *first_block = (area *)((char *)ptr[0] - align_up(sizeof(area)));
	assert(first_block->next != NULL);

	area *temp = first_block;
	while (temp) {
		assert(temp->marker == BLOCK_MARKER);

		if (temp->next != NULL) {
			assert(temp->in_use == true);
		} else {
			assert(temp->in_use == false);
		}

		temp = temp->next;
	}

	printf("After allocation: ");
	display_ll(first_block);

	my_stats *malloc_header = get_malloc_header();
	printf("Number of blocks: %zu\n", malloc_header->blocks);
	printf("Number of pages: %zu\n", malloc_header->pages);

	printf("\nFreeing every alternate block.\n");
	for (int i = 0; i < 100; i += 2) {
		an_free(ptr[i]);
	}
	printf("After freeing: ");
	display_ll(first_block);
	printf("Number of blocks: %zu\n", malloc_header->blocks);
	printf("Number of pages: %zu\n", malloc_header->pages);

	int arr2[50] = {
		1468, 2116, 1549, 2351, 3241, 686, 1335, 3969, 3509, 2215,
		3895, 2812, 4083, 980, 1111, 28, 14, 2211, 3614, 2456,
		619, 2124, 3789, 296, 3705, 3563, 689, 149, 185, 3741,
		2070, 3281, 2086, 4057, 2497, 2276, 1128, 2535, 1438, 2189,
		2045, 3115, 3755, 2325, 1006, 3886, 502, 593, 1469, 3910
	};

	for (int i = 0; i < 50; i++) {
		ptr[100 + i] = (char *)an_malloc((size_t)arr2[i]);
	}

	printf("After 50 more blocks: ");
	display_ll(first_block);
	printf("Number of blocks: %zu\n", malloc_header->blocks);
	printf("Number of pages: %zu\n", malloc_header->pages);

	printf("Free every block.\n");
	for (int i = 0; i < 150; i++) {
		an_free(ptr[i]);
	}
	display_ll(first_block);
	printf("Number of blocks: %zu\n", malloc_header->blocks);
	printf("Number of pages: %zu\n", malloc_header->pages);
}

void test_reduce_heap_size(void) {
	char *ptr = (char *)an_malloc(128);
	char *ptr1 = (char *)an_malloc(64);
	char *ptr2 = (char *)an_malloc(256);
	char *ptr3 = (char *)an_malloc(1024);

	char *ptr4 = (char *)an_malloc(12228);

	area *first_block = (area *)((char *)ptr - align_up(sizeof(area)));

	printf("After allocation: ");
	display_ll(first_block);
	my_stats *malloc_header = get_malloc_header();
	printf("Number of blocks: %zu\n", malloc_header->blocks);
	printf("Number of pages: %zu\n", malloc_header->pages);

	void *heap_before_free = sbrk_return_current_break();
	an_free(ptr4);
	void *heap_after_free = sbrk_return_current_break();
	assert((char *)heap_before_free - (char *)heap_after_free == 3 * PAGE_SIZE);

	printf("After freeing: ");
	display_ll(first_block);
	printf("Number of blocks: %zu\n", malloc_header->blocks);
	printf("Number of pages: %zu\n", malloc_header->pages);

	ptr4 = (char *)an_malloc(4096);

	printf("After allocating again: ");
	display_ll(first_block);
	printf("Number of blocks: %zu\n", malloc_header->blocks);
	printf("Number of pages: %zu\n", malloc_header->pages);
}

void call_test(void (*test_func)(), const char *msg) {
	pid_t pid = fork();
	if (!pid) {
		test_func();
		exit(0);
	}
	int status;
	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status))
		printf("%s crashed with signal %d\n", msg, WTERMSIG(status));
	else
		printf("%s passed.\n", msg);
}

int main() {
	printf("Running my Memory Allocator.\n");
	call_test(test_basic_malloc, "Basic Malloc");
	call_test(test_free, "Basic Free");
	call_test(test_reduce_heap_size, "Basic Heap Reduction");
	return 0;
}