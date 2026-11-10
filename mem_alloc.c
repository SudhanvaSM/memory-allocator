#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>

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

typedef struct stats {
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
	if (size > 0) {
		void *prev_break = sbrk(size);
		if (prev_break == (void *) -1) {
			perror("sbrk heap increase failed.\n");
			return 1;
		}
		else return 0;
	}

	else if (size < 0) {
		void *prev_break = sbrk(size);
		if (prev_break == (void *) -1) {
			perror("sbrk heap reduction failed.\n");
			return 1;
		}
		else return 0;
	}

	else return 1;
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

void reduce_heap_size() {
	area *last_block = find_last_block();
	area *prev_used_block = find_previous_used_block(last_block);
	if (!prev_used_block) {
		if (last_block->length > PAGE_SIZE) last_block->length = PAGE_SIZE;
		prev_used_block = last_block;
	}
	void *new_end = (char *)prev_used_block + align_up(sizeof(area)) + prev_used_block->length;
	void *heap_end = sbrk_return_current_break();
	my_stats *malloc_header = get_malloc_header();
	while (new_end < heap_end - PAGE_SIZE) {
		if (sbrk_step(-PAGE_SIZE) != 0) {
			perror("heap reduction failed.\n");
			return;
		}
		heap_end = sbrk_return_current_break();
		malloc_header->pages -= 1;
	}

	if (heap_end - new_end > align_up(sizeof(area)) + 1) {
		area *new_not_used_block = (area *)new_end;
		new_not_used_block->marker = BLOCK_MARKER;
		new_not_used_block->in_use = false;
		new_not_used_block->length = heap_end - new_end - align_up(sizeof(area));
		new_not_used_block->next = NULL;
		new_not_used_block->prev = prev_used_block;
		prev_used_block->next = new_not_used_block;
	}
}

int *an_malloc(ssize_t size) {
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

bool check_block_exists(void *ptr) {
	my_stats *malloc_header = get_malloc_header();
	area *temp = (area *)((char *)malloc_header + align_up(sizeof(my_stats)));
	area *block_to_free = (area *)((char *)ptr - align_up(sizeof(area)));
	while (temp->next) {
		if (temp == block_to_free) return true;
		temp = temp->next;
	}
	return false;
}

bool an_free(void *ptr) {
	my_stats *malloc_header = get_malloc_header();
	while(malloc_header->lock) sleep(1);
	malloc_header->lock = true;

	if (!check_block_exists(ptr)) {
		malloc_header->lock = false;
		return false;
	}
	area *block = (area *)((char *)ptr - align_up(sizeof(area)));
	if (block->marker != BLOCK_MARKER) {
		malloc_header->lock = false;
		return false;
	}

	if (!block->in_use) {
		malloc_header->lock = false;
		return false;
	}
	block->in_use = false;
	memset(ptr, 0, block->length);
	if (block->next && !(block->next)->in_use) {
		area *not_used_next_block = block->next;
		if (not_used_next_block) {
			block->next = not_used_next_block->next;
			if (not_used_next_block->next) not_used_next_block->next->prev = block;
			else block->next = NULL;
		}
		block->length += align_up(sizeof(area)) + not_used_next_block->length;
		memset((void *)not_used_next_block, 0, sizeof(area) + not_used_next_block->length);
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
	reduce_heap_size();
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
	// call_test(test_free, "Basic Free");
	return 0;
}