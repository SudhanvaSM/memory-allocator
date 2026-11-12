/*
 * test_mem_alloc.c - full test suite for the sbrk-based allocator in mem_alloc.c
 *
 * Build:   gcc -g -O0 -Wall -o bin/test_mem_alloc test_mem_alloc.c
 * Run:     ./bin/test_mem_alloc            (all tests)
 *          ./bin/test_mem_alloc coalesce   (only tests whose name contains "coalesce")
 *          ./bin/test_mem_alloc -l         (list tests)
 *
 * To test a different source file:
 *          gcc -DALLOCATOR_SRC='"other.c"' ...
 *
 * Design notes
 *  - The allocator source is #included, so tests can inspect internals
 *    (heap_start, area, my_stats). Its main() is renamed out of the way.
 *  - Every test runs in a forked child, so each starts with a pristine
 *    allocator (allocator_initialized == false). Crashes, asserts and hangs
 *    (alarm) are reported per test instead of killing the whole run.
 *  - Tests must NOT call libc malloc after the allocator is initialised
 *    (libc would move the program break underneath the allocator). All test
 *    storage is static.
 *  - check_invariants() is run after every allocator call. It is the main
 *    safety net: it validates the whole heap layout, not just one value.
 */
#include <stdbool.h>
#include <stdarg.h>
#include <limits.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/resource.h>

#ifndef ALLOCATOR_SRC
#define ALLOCATOR_SRC "mem_alloc.c"
#endif

#define main allocator_demo_main
#include ALLOCATOR_SRC
#undef main

#define SKIP_EXIT 77

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static size_t HDR(void)  { return align_up(sizeof(area)); }
static size_t SHDR(void) { return align_up(sizeof(my_stats)); }
static size_t PG(void)   { return (size_t)PAGE_SIZE; }

static my_stats *stats(void)      { return (my_stats *)heap_start; }
static area *first_area(void)     { return (area *)(heap_start + SHDR()); }
static area *area_of(const void *p) { return (area *)((char *)p - HDR()); }
static char *brk_now(void)        { return (char *)sbrk(0); }
static size_t heap_bytes(void)    { return (size_t)(brk_now() - heap_start); }

static area *tail_area(void) {
	area *a = first_area();
	while (a->next) a = a->next;
	return a;
}

static void dump_list(void) {
	if (!allocator_initialized || !heap_start) return;
	fprintf(stderr, "    heap: %zu bytes, stats{blocks=%zu pages=%zu lock=%d}\n    list: ",
	        heap_bytes(), stats()->blocks, stats()->pages, (int)stats()->lock);
	area *a = first_area();
	for (int i = 0; a && i < 40; i++, a = a->next)
		fprintf(stderr, "%zu(%s)%s", a->length, a->in_use ? "U" : "F", a->next ? " -> " : "");
	fprintf(stderr, "%s\n", a ? " ..." : "");
}

#define FAIL(...) do {                                              \
		fprintf(stderr, "    FAIL %s:%d: ", __FILE__, __LINE__);    \
		fprintf(stderr, __VA_ARGS__);                               \
		fprintf(stderr, "\n");                                      \
		dump_list();                                                \
		_exit(1);                                                   \
	} while (0)

#define CHECK(cond, ...) do { if (!(cond)) FAIL(__VA_ARGS__); } while (0)

#define SKIP(msg) do { fprintf(stderr, "    SKIP: %s\n", msg); _exit(SKIP_EXIT); } while (0)

/* Pattern fill / verify, to detect overlap and corruption. */
static void fill(void *p, size_t n, unsigned seed) {
	unsigned char *c = p;
	for (size_t i = 0; i < n; i++) c[i] = (unsigned char)(seed * 31u + i * 7u + (i >> 8));
}
static bool verify(const void *p, size_t n, unsigned seed) {
	const unsigned char *c = p;
	for (size_t i = 0; i < n; i++)
		if (c[i] != (unsigned char)(seed * 31u + i * 7u + (i >> 8))) return false;
	return true;
}

/* ------------------------------------------------------------------ */
/* Whole-heap invariants                                               */
/* ------------------------------------------------------------------ */

static void check_invariants(const char *where) {
	CHECK(allocator_initialized && heap_start, "%s: allocator not initialised", where);
	my_stats *st = stats();
	CHECK(st->init_marker == INIT_MARKER, "%s: stats header marker corrupted", where);
	CHECK(!st->lock, "%s: lock left held after call returned", where);

	size_t span = heap_bytes();
	CHECK(span % PG() == 0,
	      "%s: heap size %zu is not a page multiple (break is off by %zu bytes)",
	      where, span, span % PG());
	CHECK(st->pages == span / PG(),
	      "%s: stats->pages=%zu but heap is %zu pages", where, st->pages, span / PG());

	area *prev = NULL, *b = first_area();
	char *expect = heap_start + SHDR();
	size_t n = 0;
	while (b) {
		CHECK((char *)b == expect,
		      "%s: block #%zu at wrong address (gap/overlap of %td bytes)",
		      where, n, (char *)b - expect);
		CHECK(b->marker == BLOCK_MARKER, "%s: block #%zu marker corrupted", where, n);
		CHECK(b->prev == prev, "%s: block #%zu has bad prev pointer", where, n);
		CHECK(b->length <= span,
		      "%s: block #%zu length %zu is absurd (size_t underflow?)", where, n, b->length);
		if (b->in_use)
			CHECK(((uintptr_t)b + HDR()) % ALIGNMENT == 0,
			      "%s: block #%zu payload not %zu-byte aligned", where, n, (size_t)ALIGNMENT);
		if (prev)
			CHECK(prev->in_use || b->in_use,
			      "%s: blocks #%zu and #%zu are both free (missed coalesce)", where, n - 1, n);
		expect = (char *)b + HDR() + b->length;
		CHECK(expect <= heap_start + span, "%s: block #%zu runs past the break", where, n);
		prev = b;
		b = b->next;
		n++;
		CHECK(n < 1000000, "%s: list has a cycle", where);
	}
	CHECK(expect == heap_start + span,
	      "%s: last block ends %td bytes away from the program break", where,
	      (heap_start + span) - expect);
	CHECK(!prev->in_use, "%s: last block must always be free", where);
	CHECK(st->blocks == n, "%s: stats->blocks=%zu but list has %zu blocks", where, st->blocks, n);
}

/* State of an allocator with nothing allocated: one block, one page. */
static void check_baseline(const char *where) {
	check_invariants(where);
	CHECK(stats()->blocks == 1, "%s: expected 1 block, got %zu", where, stats()->blocks);
	CHECK(stats()->pages == 1, "%s: expected 1 page, got %zu", where, stats()->pages);
	area *f = first_area();
	CHECK(!f->in_use && !f->next, "%s: single block should be free and last", where);
	CHECK(f->length == PG() - SHDR() - HDR(),
	      "%s: baseline length %zu, expected %zu", where, f->length, PG() - SHDR() - HDR());
}

static void *must_malloc(size_t n) {
	void *p = an_malloc((ssize_t)n);
	CHECK(p != NULL, "an_malloc(%zu) returned NULL", n);
	CHECK(((uintptr_t)p) % ALIGNMENT == 0, "an_malloc(%zu) returned misaligned %p", n, p);
	CHECK(area_of(p)->in_use && area_of(p)->length >= n, "an_malloc(%zu): bad block header", n);
	check_invariants("after an_malloc");
	return p;
}

static void must_free(void *p) {
	CHECK(an_free(p), "an_free(%p) returned false for a live block", p);
	check_invariants("after an_free");
	/* shrink must be maximal: no whole page left to release in the tail */
	CHECK(tail_area()->length < PG(),
	      "after an_free: tail still holds %zu bytes (>= 1 page) - heap not shrunk",
	      tail_area()->length);
}

/* ------------------------------------------------------------------ */
/* Tests: argument validation                                          */
/* ------------------------------------------------------------------ */

static void t_invalid_sizes(void) {
	CHECK(an_malloc(0) == NULL, "malloc(0) must return NULL");
	CHECK(an_malloc(-1) == NULL, "malloc(-1) must return NULL");
	CHECK(an_malloc(-4096) == NULL, "malloc(-4096) must return NULL");
	CHECK(an_malloc((ssize_t)(-SSIZE_MAX - 1)) == NULL, "malloc(SSIZE_MIN) must return NULL");
	CHECK(!allocator_initialized, "invalid requests must not initialise the heap");
	CHECK(!an_free(NULL), "free(NULL) must return false");
}

static void t_huge_requests(void) {
	void *keep = must_malloc(100);
	size_t pages0 = stats()->pages, blocks0 = stats()->blocks;
	ssize_t huge[] = { SSIZE_MAX, SSIZE_MAX / 2, (ssize_t)1 << 62, (ssize_t)1 << 50 };
	for (size_t i = 0; i < sizeof huge / sizeof huge[0]; i++) {
		errno = 0;
		void *r = an_malloc(huge[i]);
		CHECK(r == NULL, "an_malloc(%zd) should fail", huge[i]);
		CHECK(errno == ENOMEM, "an_malloc(%zd) should set errno=ENOMEM, got %d", huge[i], errno);
		check_invariants("after failed huge malloc");
		CHECK(stats()->pages == pages0 && stats()->blocks == blocks0,
		      "failed malloc changed stats");
	}
	/* allocator is still fully usable */
	void *p = must_malloc(200);
	fill(p, 200, 1);
	must_free(p);
	must_free(keep);
	check_baseline("end");
}

/* ------------------------------------------------------------------ */
/* Tests: basic malloc                                                 */
/* ------------------------------------------------------------------ */

static void t_first_malloc(void) {
	char *brk_before = brk_now();
	void *p = must_malloc(1);
	CHECK(heap_start == brk_before, "heap_start should be the break at first call");
	CHECK(heap_bytes() == PG(), "fresh heap should be exactly one page, got %zu", heap_bytes());
	CHECK(stats()->blocks == 2 && stats()->pages == 1, "expected 2 blocks / 1 page");
	CHECK(area_of(p) == first_area(), "first allocation must be the first block");
	CHECK(area_of(p)->length == align_up(1), "length should be rounded up to alignment");
	CHECK(tail_area()->length == PG() - SHDR() - 2 * HDR() - align_up(1), "tail length wrong");
	must_free(p);
	check_baseline("end");
}

static void t_alignment_and_rounding(void) {
	static const size_t sizes[] = {1,2,7,8,15,16,17,31,32,33,47,48,49,63,64,65,100,127,128,129,
	                               255,256,257,1000,1023,1024,1025,4095,4096,4097,9999};
	enum { N = sizeof sizes / sizeof sizes[0] };
	static void *p[N];
	for (size_t i = 0; i < N; i++) {
		p[i] = must_malloc(sizes[i]);
		CHECK(area_of(p[i])->length == align_up(sizes[i]),
		      "size %zu: block length %zu, expected %zu", sizes[i], area_of(p[i])->length,
		      align_up(sizes[i]));
		fill(p[i], sizes[i], (unsigned)i);   /* writes every requested byte */
	}
	for (size_t i = 0; i < N; i++)
		CHECK(verify(p[i], sizes[i], (unsigned)i), "size %zu block was overwritten", sizes[i]);
	for (size_t i = 0; i < N; i++) must_free(p[i]);
	check_baseline("end");
}

static void t_no_overlap(void) {
	enum { N = 64 };
	static void *p[N];
	static size_t sz[N];
	for (int i = 0; i < N; i++) {
		sz[i] = (size_t)((i * 37) % 700) + 1;
		p[i] = must_malloc(sz[i]);
		fill(p[i], sz[i], (unsigned)i);
		CHECK((char *)p[i] >= heap_start && (char *)p[i] + sz[i] <= brk_now(),
		      "block %d lies outside the heap", i);
	}
	for (int i = 0; i < N; i++) {
		CHECK(verify(p[i], sz[i], (unsigned)i), "block %d corrupted by a later allocation", i);
		for (int j = i + 1; j < N; j++)
			CHECK((char *)p[i] + sz[i] <= (char *)p[j] || (char *)p[j] + sz[j] <= (char *)p[i],
			      "blocks %d and %d overlap", i, j);
	}
	for (int i = 0; i < N; i++) must_free(p[i]);
	check_baseline("end");
}

static void t_multi_page_growth(void) {
	static const size_t sizes[] = {1, 4016, 4017, 4096, 10000, 65536, 100000};
	for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
		size_t s = sizes[i];
		void *p = must_malloc(s);
		/* heap must be the minimal page multiple that holds: stats + block + tail header + min tail */
		size_t need = SHDR() + 2 * HDR() + align_up(s) + MIN_PAYLOAD;
		size_t want_pages = align_page_up(need) / PG();
		if (want_pages < 1) want_pages = 1;
		CHECK(stats()->pages == want_pages, "size %zu: pages=%zu, expected %zu", s,
		      stats()->pages, want_pages);
		fill(p, s, (unsigned)i);
		CHECK(verify(p, s, (unsigned)i), "size %zu: data not intact", s);
		must_free(p);
		check_baseline("after freeing multi-page block");
	}
}

/* ------------------------------------------------------------------ */
/* Tests: free validation                                              */
/* ------------------------------------------------------------------ */

static void t_invalid_free(void) {
	void *a = must_malloc(64), *g = must_malloc(64);
	int local = 0;
	CHECK(!an_free(NULL), "free(NULL)");
	CHECK(!an_free((char *)a + 1), "free(interior+1) must be rejected");
	CHECK(!an_free((char *)a + 16), "free(interior+16) must be rejected");
	CHECK(!an_free((char *)a - 1), "free(payload-1) must be rejected");
	CHECK(!an_free(&local), "free(stack pointer) must be rejected");
	CHECK(!an_free(heap_start), "free(stats header) must be rejected");
	check_invariants("after rejected frees");
	CHECK(area_of(a)->in_use && area_of(g)->in_use, "rejected frees must not change blocks");

	must_free(a);                          /* neighbours: none / used -> no merge */
	size_t blocks = stats()->blocks;
	CHECK(!an_free(a), "double free (unmerged block) must be rejected");
	check_invariants("after double free");
	CHECK(stats()->blocks == blocks, "double free changed block count");

	must_free(g);                          /* merges with a and tail */
	CHECK(!an_free(g), "double free (merged-away block) must be rejected");
	CHECK(!an_free(a), "double free (surviving merged block) must be rejected");
	check_baseline("end");
}

static void t_header_corruption_detected_on_free(void) {
	void *a = must_malloc(64), *g = must_malloc(64);
	area *h = area_of(a);
	h->marker ^= 0xFF;
	CHECK(!an_free(a), "free of a block with a smashed marker must be rejected");
	CHECK(!stats()->lock, "lock must be released after rejected free");
	h->marker ^= 0xFF;
	must_free(a);
	must_free(g);
	check_baseline("end");
}

/* ------------------------------------------------------------------ */
/* Tests: coalescing                                                   */
/* ------------------------------------------------------------------ */

static void t_coalesce_all_cases(void) {
	void *A = must_malloc(64), *B = must_malloc(64), *C = must_malloc(64);
	void *D = must_malloc(64), *G = must_malloc(16);
	CHECK(stats()->blocks == 6, "setup: expected 6 blocks");

	must_free(B);                                  /* both neighbours used */
	CHECK(stats()->blocks == 6 && area_of(B)->length == 64, "free B: no merge expected");
	must_free(D);                                  /* neighbours C and G used */
	CHECK(stats()->blocks == 6 && area_of(D)->length == 64, "free D: no merge expected");

	must_free(C);                                  /* merges next (D) and prev (B) */
	CHECK(stats()->blocks == 4, "free C: expected 4 blocks, got %zu", stats()->blocks);
	CHECK(area_of(B)->length == 3 * 64 + 2 * HDR(), "free C: merged length %zu wrong",
	      area_of(B)->length);
	CHECK(area_of(B)->next == area_of(G) && area_of(G)->prev == area_of(B),
	      "free C: links around merged block are wrong");

	must_free(A);                                  /* merges next only */
	CHECK(stats()->blocks == 3, "free A: expected 3 blocks");
	CHECK(area_of(A)->length == 4 * 64 + 3 * HDR(), "free A: merged length wrong");
	CHECK(area_of(A)->prev == NULL && area_of(A)->next == area_of(G), "free A: links wrong");

	must_free(G);                                  /* merges prev and tail */
	check_baseline("end");
}

static void t_coalesce_prev_only(void) {
	void *X = must_malloc(64), *Y = must_malloc(64), *G = must_malloc(64);
	must_free(X);
	size_t b = stats()->blocks;
	must_free(Y);
	CHECK(stats()->blocks == b - 1, "prev-only merge should remove exactly one block");
	CHECK(area_of(X)->length == 2 * 64 + HDR(), "prev-only merged length wrong");
	CHECK(area_of(X)->next == area_of(G) && area_of(G)->prev == area_of(X), "links wrong");
	must_free(G);
	check_baseline("end");
}

static void t_coalesce_next_only(void) {
	void *X = must_malloc(64), *Y = must_malloc(64), *G = must_malloc(64);
	must_free(Y);
	size_t b = stats()->blocks;
	must_free(X);
	CHECK(stats()->blocks == b - 1, "next-only merge should remove exactly one block");
	CHECK(area_of(X)->length == 2 * 64 + HDR(), "next-only merged length wrong");
	CHECK(area_of(X)->prev == NULL && area_of(X)->next == area_of(G), "links wrong");
	must_free(G);
	check_baseline("end");
}

static void t_free_orders(void) {
	enum { N = 42 };
	static void *p[N];
	static size_t sz[N];
	static bool live[N];
	for (int order = 0; order < 5; order++) {
		for (int i = 0; i < N; i++) {
			sz[i] = (size_t)((i * 131) % 900) + 1;
			if (i == 20) sz[i] = 7000;      /* spans pages */
			if (i == N - 1) sz[i] = 3000;
			p[i] = must_malloc(sz[i]);
			fill(p[i], sz[i], (unsigned)(i + order));
			live[i] = true;
		}
		for (int k = 0; k < N; k++) {
			int i;
			switch (order) {
			case 0:  i = k; break;                                   /* forward   */
			case 1:  i = N - 1 - k; break;                           /* reverse   */
			case 2:  i = (k < N / 2) ? 2 * k : 2 * (k - N / 2) + 1; break; /* evens, odds */
			case 3:  i = (k < N / 2) ? 2 * k + 1 : 2 * (k - N / 2); break; /* odds, evens */
			default: i = (k % 2) ? (N / 2 + k / 2) % N : (N / 2 - 1 - k / 2 + N) % N; break;
			}
			if (!live[i]) { /* order 4 formula may repeat; fall back to first live */
				int j = 0; while (j < N && !live[j]) j++;
				if (j == N) break;
				i = j;
			}
			CHECK(verify(p[i], sz[i], (unsigned)(i + order)), "order %d: block %d corrupted", order, i);
			must_free(p[i]);
			live[i] = false;
		}
		for (int i = 0; i < N; i++) if (live[i]) { must_free(p[i]); live[i] = false; }
		check_baseline("after freeing everything");
	}
}

/* ------------------------------------------------------------------ */
/* Tests: placement policy (best fit, splitting)                       */
/* ------------------------------------------------------------------ */

static void t_best_fit(void) {
	void *A = must_malloc(208), *g1 = must_malloc(16);
	void *B = must_malloc(112), *g2 = must_malloc(16);
	void *C = must_malloc(320), *g3 = must_malloc(16);
	must_free(A); must_free(B); must_free(C);        /* three isolated holes: 208, 112, 320 */
	size_t blocks = stats()->blocks;

	void *r1 = must_malloc(96);                      /* smallest hole that fits is B (112) */
	CHECK(r1 == B, "best fit should pick the 112-byte hole");
	CHECK(area_of(B)->length == 112 && stats()->blocks == blocks,
	      "112-byte hole cannot be split for 96 bytes; should be handed out whole");

	void *r2 = must_malloc(200);                     /* A (208) beats C (320) and the tail */
	CHECK(r2 == A, "best fit should pick the 208-byte hole");

	void *r3 = must_malloc(100);                     /* C (320) beats the tail; splits */
	CHECK(r3 == C, "best fit should pick the 320-byte hole");
	CHECK(area_of(C)->length == 112 && stats()->blocks == blocks + 1,
	      "split of 320-byte hole wrong");
	CHECK(area_of(C)->next->length == 320 - 112 - HDR() && !area_of(C)->next->in_use,
	      "remainder of split has wrong length");

	void *r4 = must_malloc(160);                     /* exactly the remainder */
	CHECK(r4 == (char *)C + 112 + HDR(), "exact-fit remainder should be reused");

	must_free(r1); must_free(r2); must_free(r3); must_free(r4);
	must_free(g1); must_free(g2); must_free(g3);
	check_baseline("end");
}

static void t_split_boundaries(void) {
	size_t min = HDR() + MIN_PAYLOAD;               /* 64 with 48-byte header */
	/* hole of exactly (req + min) must split; hole one alignment step smaller must not */
	size_t req = 48;
	void *P1 = must_malloc(req + min), *g1 = must_malloc(16);
	void *P2 = must_malloc(req + min - ALIGNMENT), *g2 = must_malloc(16);
	must_free(P1); must_free(P2);
	size_t blocks = stats()->blocks;

	void *m1 = must_malloc(req);                    /* best fit -> smaller hole P2: no split */
	CHECK(m1 == P2, "should pick the smaller hole");
	CHECK(area_of(P2)->length == req + min - ALIGNMENT && stats()->blocks == blocks,
	      "hole one step below the split threshold must be handed out whole");

	void *m2 = must_malloc(req);                    /* hole P1: exactly at threshold */
	CHECK(m2 == P1, "should pick the remaining hole");
	CHECK(area_of(P1)->length == req, "threshold hole should be split to request size");
	CHECK(area_of(P1)->next->length == MIN_PAYLOAD && !area_of(P1)->next->in_use,
	      "split remainder should be exactly MIN_PAYLOAD bytes");
	CHECK(stats()->blocks == blocks + 1, "split should add exactly one block");

	must_free(m1); must_free(m2); must_free(g1); must_free(g2);
	check_baseline("end");
}

static void t_exact_fit_reuse(void) {
	void *X = must_malloc(64), *g = must_malloc(16);
	must_free(X);
	void *Y = must_malloc(64);
	CHECK(Y == X, "freed block of identical size should be reused");
	CHECK(area_of(Y)->length == 64, "reused block length changed");
	must_free(Y); must_free(g);
	check_baseline("end");
}

/* ------------------------------------------------------------------ */
/* Tests: heap shrinking (the reduce_heap_size_if_possible logic)      */
/* ------------------------------------------------------------------ */

static void t_shrink_releases_whole_pages(void) {
	/* identical scenario to your hand-written test, now with assertions */
	char *p0 = must_malloc(128), *p1 = must_malloc(64), *p2 = must_malloc(256), *p3 = must_malloc(1024);
	char *p4 = must_malloc(12228);
	fill(p0, 128, 1); fill(p1, 64, 2); fill(p2, 256, 3); fill(p3, 1024, 4); fill(p4, 12228, 5);

	CHECK(stats()->blocks == 6 && stats()->pages == 4, "setup: blocks=%zu pages=%zu",
	      stats()->blocks, stats()->pages);
	char *before = brk_now();
	must_free(p4);
	char *after = brk_now();

	CHECK((size_t)(before - after) == 3 * PG(),
	      "break should drop by exactly 3 pages, dropped %td bytes", before - after);
	CHECK(stats()->pages == 1 && stats()->blocks == 5, "after free: blocks=%zu pages=%zu",
	      stats()->blocks, stats()->pages);
	CHECK(tail_area()->length == PG() - SHDR() - 4 * HDR() - (128 + 64 + 256 + 1024) - HDR(),
	      "tail length after shrink wrong: %zu", tail_area()->length);
	CHECK(verify(p0, 128, 1) && verify(p1, 64, 2) && verify(p2, 256, 3) && verify(p3, 1024, 4),
	      "live blocks were damaged by the shrink");

	/* grow again after shrinking */
	char *p5 = must_malloc(4096);
	fill(p5, 4096, 6);
	CHECK(stats()->pages == 2 && stats()->blocks == 6, "regrow: blocks=%zu pages=%zu",
	      stats()->blocks, stats()->pages);
	CHECK(verify(p0, 128, 1) && verify(p3, 1024, 4) && verify(p5, 4096, 6),
	      "data damaged after regrow");

	must_free(p5); must_free(p3); must_free(p2); must_free(p1); must_free(p0);
	check_baseline("end");
}

static void t_shrink_not_needed(void) {
	void *a = must_malloc(1000), *b = must_malloc(500);
	char *before = brk_now();
	size_t pages = stats()->pages;
	must_free(b);                                    /* tail < 1 page: nothing to release */
	CHECK(brk_now() == before && stats()->pages == pages, "heap shrank when it should not have");

	/* freeing a block that is not adjacent to the tail must not move the break */
	void *c = must_malloc(100), *big = must_malloc(20000);
	before = brk_now(); pages = stats()->pages;
	must_free(c);
	CHECK(brk_now() == before && stats()->pages == pages,
	      "freeing a middle block moved the break");
	must_free(big);
	must_free(a);
	check_baseline("end");
}

static void t_shrink_tail_exact_page_multiple(void) {
	/* Choose A so the tail's payload starts exactly on a page boundary. */
	size_t a_sz = PG() - SHDR() - 2 * HDR();
	void *a = must_malloc(a_sz);
	fill(a, a_sz, 9);
	void *b = must_malloc(16);
	CHECK(stats()->pages == 2, "setup: expected 2 pages, got %zu", stats()->pages);
	must_free(b);          /* tail payload is exactly 4096 -> must shrink to a 0-byte tail */
	CHECK(stats()->pages == 1, "pages=%zu, expected 1", stats()->pages);
	CHECK(tail_area()->length == 0, "tail length=%zu, expected 0", tail_area()->length);
	CHECK(verify(a, a_sz, 9), "block A damaged");

	void *c = must_malloc(16);                       /* must grow again from a 0-byte tail */
	fill(c, 16, 10);
	CHECK(verify(a, a_sz, 9) && verify(c, 16, 10), "data damaged after regrow");
	must_free(c); must_free(a);
	check_baseline("end");
}

static void t_shrink_cycles_do_not_drift(void) {
	/* with a persistent block (exercises the 'previous used block' path) */
	void *keep = must_malloc(100);
	fill(keep, 100, 3);
	char *base = brk_now();
	size_t base_pages = stats()->pages;
	for (int i = 0; i < 200; i++) {
		size_t s = (size_t)((i % 7) + 1) * 3000 + (size_t)(i % 5) * 16;
		void *p = must_malloc(s);
		fill(p, s, (unsigned)i);
		void *q = must_malloc(7);
		must_free(q);
		CHECK(verify(p, s, (unsigned)i), "cycle %d: data corrupted", i);
		must_free(p);
		CHECK(brk_now() == base && stats()->pages == base_pages,
		      "cycle %d: break drifted by %td bytes", i, brk_now() - base);
	}
	CHECK(verify(keep, 100, 3), "persistent block corrupted");
	must_free(keep);
	check_baseline("persistent-block cycles");

	/* with nothing else allocated (exercises the 'no used block' path) */
	for (int i = 0; i < 200; i++) {
		size_t s = (size_t)((i % 9) + 1) * 2500;
		void *p = must_malloc(s);
		fill(p, s, (unsigned)i);
		must_free(p);
		check_baseline("empty-heap cycle");
	}
}

/* ------------------------------------------------------------------ */
/* Tests: stress and regression                                        */
/* ------------------------------------------------------------------ */

#define SLOTS 256
static void  *s_ptr[SLOTS];
static size_t s_len[SLOTS];
static unsigned s_seed[SLOTS];

static size_t pick_size(void) {
	int r = rand() % 100;
	if (r < 60) return (size_t)(rand() % 256) + 1;
	if (r < 90) return (size_t)(rand() % 4096) + 1;
	if (r < 99) return (size_t)(rand() % 16384) + 1;
	return (size_t)(rand() % 70000) + 1;
}

static void stress(unsigned seed, int ops) {
	srand(seed);
	for (int op = 0; op < ops; op++) {
		int i = rand() % SLOTS;
		if (s_ptr[i]) {
			CHECK(verify(s_ptr[i], s_len[i], s_seed[i]), "seed %u op %d: slot %d corrupted", seed, op, i);
			must_free(s_ptr[i]);
			s_ptr[i] = NULL;
		} else {
			s_len[i] = pick_size();
			s_seed[i] = (unsigned)rand();
			s_ptr[i] = must_malloc(s_len[i]);
			fill(s_ptr[i], s_len[i], s_seed[i]);
		}
		if (op % 64 == 0)
			for (int k = 0; k < SLOTS; k++)
				if (s_ptr[k])
					CHECK(verify(s_ptr[k], s_len[k], s_seed[k]),
					      "seed %u op %d: slot %d corrupted (sweep)", seed, op, k);
	}
	for (int i = 0; i < SLOTS; i++) {
		if (!s_ptr[i]) continue;
		CHECK(verify(s_ptr[i], s_len[i], s_seed[i]), "seed %u: slot %d corrupted at end", seed, i);
		must_free(s_ptr[i]);
		s_ptr[i] = NULL;
	}
	check_baseline("after stress");
}

static void t_stress_seed1(void) { stress(1, 4000); }
static void t_stress_seed2(void) { stress(2, 4000); }
static void t_stress_seed3(void) { stress(12345, 4000); }

static void t_original_workload(void) {
	/* the workload from test_free(), with invariant checks at every step */
	static const int arr[100] = {
		3444, 346, 2017, 2506, 2526, 3538, 1802, 3453, 1508, 1406,
		647, 3270, 3564, 286, 3415, 3322, 1105, 108, 808, 1253,
		1386, 3222, 408, 3920, 2407, 2486, 1626, 738, 2690, 1651,
		206, 3017, 3368, 3620, 3397, 455, 3380, 349, 1001, 2713,
		2548, 3470, 1582, 458, 2882, 822, 1408, 712, 240, 2137,
		3880, 3611, 4011, 1159, 4079, 3026, 1640, 1136, 3485, 3778,
		7, 1105, 3461, 3790, 1169, 2846, 57, 319, 2572, 712,
		2193, 3062, 2420, 897, 3088, 3336, 2376, 1017, 2764, 1953,
		1960, 175, 2168, 1950, 1474, 417, 2901, 2416, 2075, 2809,
		3837, 2748, 3077, 3682, 631, 1455, 606, 3017, 583, 450 };
	static const int arr2[50] = {
		1468, 2116, 1549, 2351, 3241, 686, 1335, 3969, 3509, 2215,
		3895, 2812, 4083, 980, 1111, 28, 14, 2211, 3614, 2456,
		619, 2124, 3789, 296, 3705, 3563, 689, 149, 185, 3741,
		2070, 3281, 2086, 4057, 2497, 2276, 1128, 2535, 1438, 2189,
		2045, 3115, 3755, 2325, 1006, 3886, 502, 593, 1469, 3910 };
	static void *ptr[150];
	static size_t len[150];
	static bool live[150];

	for (int i = 0; i < 100; i++) {
		len[i] = (size_t)arr[i];
		ptr[i] = must_malloc(len[i]);
		fill(ptr[i], len[i], (unsigned)i);
		live[i] = true;
	}
	for (int i = 0; i < 100; i += 2) {
		CHECK(verify(ptr[i], len[i], (unsigned)i), "block %d corrupted before free", i);
		must_free(ptr[i]);
		live[i] = false;
	}
	for (int i = 1; i < 100; i += 2)
		CHECK(verify(ptr[i], len[i], (unsigned)i), "odd block %d corrupted by alternate frees", i);
	for (int i = 0; i < 50; i++) {
		len[100 + i] = (size_t)arr2[i];
		ptr[100 + i] = must_malloc(len[100 + i]);
		fill(ptr[100 + i], len[100 + i], (unsigned)(100 + i));
		live[100 + i] = true;
	}
	for (int i = 0; i < 150; i++)
		if (live[i]) CHECK(verify(ptr[i], len[i], (unsigned)i), "block %d corrupted after refill", i);
	for (int i = 0; i < 150; i++)
		if (live[i]) { must_free(ptr[i]); live[i] = false; }
	check_baseline("end of original workload");
}

/* ------------------------------------------------------------------ */
/* Tests: out-of-memory behaviour                                      */
/* ------------------------------------------------------------------ */

static void t_out_of_memory(void) {
	void *keep = must_malloc(100);
	fill(keep, 100, 4);

	struct rlimit rl;
	if (getrlimit(RLIMIT_DATA, &rl) != 0) SKIP("getrlimit failed");
	rl.rlim_cur = 1;                                 /* forbid any further brk growth */
	if (setrlimit(RLIMIT_DATA, &rl) != 0) SKIP("setrlimit failed");
	void *probe = sbrk((intptr_t)PG());
	if (probe != (void *)-1) { sbrk(-(intptr_t)PG()); SKIP("kernel does not enforce RLIMIT_DATA on brk"); }

	size_t pages0 = stats()->pages, blocks0 = stats()->blocks;

	/* 1) request larger than anything available: growth path through the tail block */
	errno = 0;
	CHECK(an_malloc(1 << 20) == NULL, "malloc beyond limit should return NULL");
	CHECK(errno == ENOMEM, "errno should be ENOMEM, got %d", errno);
	check_invariants("after failed growth (tail path)");
	CHECK(stats()->pages == pages0 && stats()->blocks == blocks0, "failed malloc changed stats");

	/* 2) request that fits the tail exactly (cannot split) -> growth through the 'smallest_block is last' path */
	errno = 0;
	CHECK(an_malloc((ssize_t)tail_area()->length) == NULL, "exact-tail request needs growth; should fail");
	check_invariants("after failed growth (exact-fit path)");
	CHECK(stats()->pages == pages0 && stats()->blocks == blocks0, "failed malloc changed stats");

	/* 3) still works within existing capacity */
	void *p = must_malloc(500);
	fill(p, 500, 5);
	CHECK(verify(keep, 100, 4) && verify(p, 500, 5), "data damaged");
	must_free(p);
	must_free(keep);
}

/* ------------------------------------------------------------------ */
/* Harness                                                             */
/* ------------------------------------------------------------------ */

typedef struct { const char *name; void (*fn)(void); } test_case;

static const test_case TESTS[] = {
	{"invalid_sizes",                     t_invalid_sizes},
	{"first_malloc",                      t_first_malloc},
	{"alignment_and_rounding",            t_alignment_and_rounding},
	{"no_overlap",                        t_no_overlap},
	{"multi_page_growth",                 t_multi_page_growth},
	{"huge_requests",                     t_huge_requests},
	{"invalid_free",                      t_invalid_free},
	{"header_corruption_detected",        t_header_corruption_detected_on_free},
	{"coalesce_all_cases",                t_coalesce_all_cases},
	{"coalesce_prev_only",                t_coalesce_prev_only},
	{"coalesce_next_only",                t_coalesce_next_only},
	{"free_orders",                       t_free_orders},
	{"best_fit",                          t_best_fit},
	{"split_boundaries",                  t_split_boundaries},
	{"exact_fit_reuse",                   t_exact_fit_reuse},
	{"shrink_releases_whole_pages",       t_shrink_releases_whole_pages},
	{"shrink_not_needed",                 t_shrink_not_needed},
	{"shrink_tail_exact_page_multiple",   t_shrink_tail_exact_page_multiple},
	{"shrink_cycles_do_not_drift",        t_shrink_cycles_do_not_drift},
	{"original_workload",                 t_original_workload},
	{"stress_seed1",                      t_stress_seed1},
	{"stress_seed2",                      t_stress_seed2},
	{"stress_seed3",                      t_stress_seed3},
	{"out_of_memory",                     t_out_of_memory},
};
#define NTESTS (sizeof TESTS / sizeof TESTS[0])

/* returns 0 pass, 1 fail, 2 skip */
static int run_one(const test_case *t) {
	printf("[ RUN  ] %s\n", t->name);
	fflush(stdout);
	pid_t pid = fork();
	if (pid < 0) { perror("fork"); return 1; }
	if (pid == 0) {
		alarm(20);                  /* a stuck lock (sleep loop) shows up as a timeout */
		t->fn();
		_exit(0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0) { printf("[ PASS ] %s\n", t->name); fflush(stdout); return 0; }
	if (WIFEXITED(st) && WEXITSTATUS(st) == SKIP_EXIT) { printf("[ SKIP ] %s\n", t->name); fflush(stdout); return 2; }
	if (WIFEXITED(st)) printf("[ FAIL ] %s (exit code %d)\n", t->name, WEXITSTATUS(st));
	else if (WTERMSIG(st) == SIGALRM) printf("[ FAIL ] %s (timed out: lock never released or infinite loop)\n", t->name);
	else if (WTERMSIG(st) == SIGABRT) printf("[ FAIL ] %s (abort: assert or fatal error inside allocator)\n", t->name);
	else printf("[ FAIL ] %s (killed by signal %d: %s)\n", t->name, WTERMSIG(st), strsignal(WTERMSIG(st)));
	fflush(stdout);
	return 1;
}

int main(int argc, char **argv) {
	if (argc > 1 && strcmp(argv[1], "-l") == 0) {
		for (size_t i = 0; i < NTESTS; i++) puts(TESTS[i].name);
		return 0;
	}
	const char *filter = argc > 1 ? argv[1] : NULL;
	printf("Allocator test suite (header: stats=%zu bytes, block=%zu bytes, page=%d)\n",
	       align_up(sizeof(my_stats)), align_up(sizeof(area)), PAGE_SIZE);
	fflush(stdout);

	int passed = 0, failed = 0, skipped = 0;
	for (size_t i = 0; i < NTESTS; i++) {
		if (filter && !strstr(TESTS[i].name, filter)) continue;
		switch (run_one(&TESTS[i])) {
		case 0: passed++; break;
		case 2: skipped++; break;
		default: failed++; break;
		}
	}
	printf("\n%d passed, %d failed, %d skipped\n", passed, failed, skipped);
	return failed ? 1 : 0;
}