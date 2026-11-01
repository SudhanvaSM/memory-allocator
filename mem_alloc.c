#include <stdio.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>

typedef struct free_area {
	uint8_t marker;
	struct free_area *prev;
	bool in_use;
	uint32_t length;
	struct free_area *next;
} area;

typedef struct stats {
	int init_marker;
	bool lock;
	uint32_t blocks;
	uint16_t pages;
} my_stats;

const int INIT_MARKER = 0x55;
const int BLOCK_MARKER = 0xDD;
const int PAGE_SIZE = 4096;
const int FIRST_BLOCK_OFFSET = sizeof(area);

char *heap_start = NULL; 

my_stats *get_malloc_header() {
	assert(heap_start != NULL);
	my_stats *malloc_header = (my_stats *)heap_start;
	assert(malloc_header->init_marker == INIT_MARKER);
	return malloc_header;
}

area *find_last_block() {
	my_stats *malloc_header = get_malloc_header();
	area *block = (area *)((char *)malloc_header + sizeof(my_stats));
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

int *add_used_block(ssize_t size) {
	my_stats *malloc_header = get_malloc_header();
	while (malloc_header->lock) sleep(1);

	malloc_header->lock = true;
	area *block = (area *)(heap_start + sizeof(my_stats));
	area *smallest_block = NULL;
	area *last_block = block;

	while (block != NULL) {
		assert(block->marker == BLOCK_MARKER);
		if (block->length >= size && !block->in_use)
			if (!smallest_block || smallest_block->length > block->length) 
				smallest_block = block;

		last_block = block;
		block = block->next;
	}
	if (!smallest_block) {
		last_block = find_last_block();
		while (last_block->length < size) {
			void *prev_break = sbrk(4096);
			if (prev_break == (void *) -1) {
				perror("sbrk failed.\n");
				malloc_header->lock = false;
				return NULL;
			}
			last_block->length += 4096;
			malloc_header->pages += 1;
		}
		smallest_block = last_block;
	}
	smallest_block->in_use = true;
	int must_have_size = smallest_block->length - size - sizeof(area) - 1;
	if (must_have_size <= 0) {
		void *prev_break = sbrk(4096);
		if (prev_break == (void *) -1) {
			perror("sbrk failed.\n");
			malloc_header->lock = false;
			return NULL;
		}
		last_block->length += 4096;
		malloc_header->pages += 1;
		must_have_size = smallest_block->length - size - sizeof(area) - 1;
	}
	int remaining_size = must_have_size + 1;
	malloc_header->blocks += 1;
	area *new_block = (area *)((char *)smallest_block + sizeof(area) + size);
	new_block->marker = BLOCK_MARKER;
	new_block->prev = smallest_block;
	new_block->next = smallest_block->next;
	if (new_block->next) (new_block->next)->prev = new_block;
	smallest_block->next = new_block;
	new_block->length = remaining_size;
	smallest_block->length = size;
	malloc_header->lock = false;

	return (int *)((char *) smallest_block + sizeof(area));
}

void reduce_heap_size() {
	area *last_block = find_last_block();
	area *prev_used_block = find_previous_used_block(last_block);
	if (!prev_used_block) {
		if (last_block->length > PAGE_SIZE) last_block->length = PAGE_SIZE;
		prev_used_block = last_block;
	}
	void *new_end = (char *)prev_used_block + sizeof(area) + prev_used_block->length;
	void *heap_end = sbrk(0);
	while (new_end < heap_end - PAGE_SIZE) {
		void *prev_break = sbrk(-PAGE_SIZE);
		if (prev_break == (void *) -1) {
			perror("sbrk failed.\n");
			exit(0);
		}
		heap_end = sbrk(0);
		my_stats *malloc_header = get_malloc_header();
		malloc_header->pages -= 1;
	}
	if (heap_end - new_end > sizeof(area) + 1) {
		area *new_not_used_block = (area *)new_end;
		new_not_used_block->marker = BLOCK_MARKER;
		new_not_used_block->in_use = false;
		new_not_used_block->length = heap_end - new_end - sizeof(area);
		new_not_used_block->next = NULL;
		new_not_used_block->prev = prev_used_block;
		prev_used_block->next = new_not_used_block;
	}
}

int *an_malloc(ssize_t size) {
	if (heap_start == NULL) {
		heap_start = sbrk(0);
		sbrk(4096);
	}
	char *heap_end = sbrk(0);
	long int length = heap_end - heap_start;
	if (*heap_start != INIT_MARKER) {
		*heap_start = INIT_MARKER;
		my_stats *malloc_header = (my_stats *)heap_start;
		malloc_header->blocks = 1;
    	malloc_header->pages = 1;

		area *first_block = (area *)(heap_start + sizeof(my_stats));
		first_block->marker = BLOCK_MARKER;
		first_block->in_use = false;
		first_block->length = length - sizeof(my_stats) - sizeof(area);
		first_block->prev = first_block->next = NULL;
	}
	return add_used_block(size);
}

bool an_free(void *ptr) {
	my_stats *malloc_header = get_malloc_header();
	while(malloc_header->lock) sleep(1);
	malloc_header->lock = true;
	area *block = (area *)((char *)ptr - sizeof(area));
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
		block->length += sizeof(area) + not_used_next_block->length;
		memset((void *)not_used_next_block, 0, sizeof(area) + not_used_next_block->length);
		malloc_header->blocks -= 1;
	}
	if (block-> prev && !(block->prev)->in_use) {
		area *to_delete_block = block;
		block = block->prev;
		block->length += sizeof(area) + to_delete_block->length;
		block->next = to_delete_block->next;
		if (block->next) block->next->prev = block;
		malloc_header->blocks -= 1;
	}
	reduce_heap_size();
	malloc_header->lock = false;
	return true;
}

void test_basic_malloc() {
	char *ptr = (char *)an_malloc(1);

    assert(ptr != NULL);

    area *first_block = (area *)((char *)ptr - sizeof(area));

    assert(first_block->marker == BLOCK_MARKER);
    assert(first_block->in_use == true);

    *ptr = 'C';
    assert(*ptr == 'C');

    printf("Basic Malloc result %p\n", (void *)ptr);

    assert(an_free(ptr));
    printf("Basic Malloc block freed.\n");

    assert(!an_free(ptr));
    printf("Double-free correctly rejected.\n");

	ptr = (char *)an_malloc(1);

    assert(ptr != NULL);

    first_block = (area *)((char *)ptr - sizeof(area));

    assert(first_block->marker == BLOCK_MARKER);
    assert(first_block->in_use == true);

    *ptr = 'C';
    assert(*ptr == 'C');

    printf("Reallocated memory at %p\n", (void *)ptr);

    assert(an_free(ptr));
    printf("Reallocated block freed.\n");
}

void test_bigger_than_available_malloc() {
    uint16_t *ptr = (uint16_t *)an_malloc(5000);
	area *first_block = (area *)((char *)ptr - sizeof(area));
	for (uint16_t i = 0; i <= 2499; i++)
		*(ptr + i) = i;

	assert(first_block->marker == BLOCK_MARKER);
	assert(*ptr == 0);
	assert(*(ptr + 2) == 2);
	assert(*(ptr + 2499) == 2499);
	// little endian valid only
	assert(*((uint8_t *)ptr + 4999) == 0x09);
	assert(*((uint8_t *)ptr + 4998) == 0xC3);
	printf("More Memory Malloc result %p\n", (void *)ptr);	
}

void test_free() {
	uint8_t *first = (uint8_t *)an_malloc(2048);
	area *first_block = (void *)first - sizeof(area);
	assert(first_block->next != NULL);
	assert(first_block->length == 2048);
	area *second_block = first_block->next;
	assert(second_block->marker == BLOCK_MARKER);
	assert(second_block->in_use == false);
	assert(second_block->next == NULL);
	assert(second_block->length == PAGE_SIZE - sizeof(my_stats) -  (2 * sizeof(area)) - first_block->length);
	an_free(first);
	assert(first_block->marker == BLOCK_MARKER);
	assert(first_block->next == NULL);
	assert(first_block->length == PAGE_SIZE - sizeof(my_stats) - sizeof(area));
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
	call_test(test_bigger_than_available_malloc, "Request more memory than Malloc");
	// call_test(test_free, "Basic Free");
	return 0;
}