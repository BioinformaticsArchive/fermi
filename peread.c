#include <stdio.h>
#include <limits.h>
#include "fermi.h"
#include "utils.h"
#include "kvec.h"

#include "khash.h"
KHASH_DECLARE(64, uint64_t, uint64_t)

typedef khash_t(64) hash64_t;

void ks_introsort_128x(size_t n, fm128_t *a);
void ks_heapup_128y(size_t n, fm128_t *a);
void ks_heapdown_128y(size_t i, size_t n, fm128_t *a);

static hash64_t *build_hash(const fmnode_v *n)
{
	hash64_t *h;
	int64_t i, n_dropped = 0, n_dups = 0;
	int j, ret;
	khint_t k, l;

	h = kh_init(64);
	for (i = 0; i < n->n; ++i) {
		fmnode_t *p = &n->a[i];
		for (j = 0; j < p->mapping.n; ++j) {
			k = kh_put(64, h, p->mapping.a[j].x, &ret);
			kh_val(h, k) = ret == 0? (uint64_t)-1 : (i<<1|(p->mapping.a[j].y&1))^1;
		}
	}
	for (k = kh_begin(h); k != kh_end(h); ++k) {
		int to_drop = 0;
		if (!kh_exist(h, k)) continue;
		if (kh_val(h, k) != (uint64_t)-1) {
			l = kh_get(64, h, kh_key(h, k)^1);
			if (l == k) to_drop = 1;
			else if (l != kh_end(h)) {
				if (kh_val(h, l) == (uint64_t)-1) to_drop = 1;
			} else to_drop = 1;
		} else to_drop = 1, ++n_dups;
		if (to_drop) {
			kh_del(64, h, k);
			++n_dropped;
		}
	}
	if (fm_verbose >= 3)
		fprintf(stderr, "[%s] dropped %ld reads, including %ld duplicates; %d reads remain\n",
				__func__, (long)n_dropped, (long)n_dups, kh_size(h));
	return h;
}

static void collect_pairs(fmnode_v *n, const hash64_t *h, fm128_v *pairs) // n->a[].aux to be modified
{
	size_t i, m;
	int j;
	khint_t k;
	fm128_t z, *r;
	pairs->n = 0;
	for (i = 0; i < n->n; ++i) {
		fmnode_t *p = &n->a[i];
		p->aux[0] = p->aux[1] = INT_MAX;
		for (j = 0; j < p->mapping.n; ++j) {
			k = kh_get(64, h, p->mapping.a[j].x);
			if (k == kh_end(h)) continue;
			z.x = kh_val(h, k)<<8;
			k = kh_get(64, h, p->mapping.a[j].x^1);
			z.y = kh_val(h, k);
			if (z.x>>8 < z.y) kv_push(fm128_t, *pairs, z);
		}
	}
	ks_introsort_128x(pairs->n, pairs->a);
	r = &pairs->a[0];
	for (i = 1; i < pairs->n; ++i) {
		fm128_t *q = &pairs->a[i];
		if (q->x>>8 == r->x>>8 && q->y == r->y) {
			if ((r->x&0xff) != 0xff) ++r->x;
			q->x = q->y = (uint64_t)-1;
		} else r = q, ++r->x;
	}
	for (i = m = 0; i < pairs->n; ++i)
		if (pairs->a[i].x != (uint64_t)-1 && (pairs->a[i].x&0xff) > 1)
			pairs->a[m++] = pairs->a[i];
	pairs->n = m;
	for (i = 0; i < pairs->n; ++i) {
		printf("%lld, %lld, %lld\n", pairs->a[i].x>>8, pairs->a[i].y, pairs->a[i].x&0xff);
	}
}

static inline uint64_t get_idd(hash64_t *h, uint64_t k)
{
	khint_t iter;
	iter = kh_get(64, h, k);
	return iter == kh_end(h)? (uint64_t)-1 : kh_val(h, iter);
}

typedef struct {
	fm128_v heap, stack, rst;
	fm64_v walk;
} aux_t;

static int walk(msg_t *g, const hash64_t *h, size_t idd[2], int max_dist, aux_t *a)
{ // FIXME: the algorithm can be improved but will be more complicated.
	fm128_t *q;
	fm128_v *r;
	fmnode_t *p, *w;
	int i, n_nei[2], is_multi = 0;
	uint64_t end, start;

	// stack -- .x: id+direction (idd); .y: parent
	// heap  -- .x: position in stack;  .y: distance from the end of start (can be negative)

	// initialize
	a->heap.n = a->stack.n = a->rst.n = a->walk.n = 0;
	for (i = 0; i < 2; ++i) {
		p = &g->nodes.a[i];
		n_nei[i] = p->nei[idd[i]&1].n;
	}
	if (n_nei[0] < n_nei[1]) start = idd[0], end = idd[1];
	else start = idd[1], end = idd[0];
	kv_pushp(fm128_t, a->stack, &q);
	q->x = start, q->y = (uint64_t)-1;
	kv_pushp(fm128_t, a->heap, &q);
	q->x = 0, q->y = -g->nodes.a[start>>1].l; // note that "128y" compares int64_t instead of uint64_t
	// shortest path
	while (a->heap.n) {
		// pop up the best node
		fm128_t z = a->heap.a[0];
		a->heap.a[0] = a->heap.a[--a->heap.n];
		ks_heapdown_128y(0, a->heap.n, a->heap.a);
		// push to the heap
		p = &g->nodes.a[a->stack.a[z.x].x>>1];
		r = &p->nei[a->stack.a[z.x].x&1];
		for (i = 0; i < r->n; ++i) {
			uint64_t u = get_idd(g->h, r->a[i].x);
			w = &g->nodes.a[u>>1];
			if (w->aux[0] != INT_MAX) { // visited before
				if (++w->aux[0] >= 3) break;
				is_multi = 1; // multiple paths
			} else if ((int64_t)z.y + p->l - r->a[i].y < max_dist) { // then push to the heap
				int64_t dist = (int64_t)z.y + p->l - r->a[i].y;
				kv_pushp(fm128_t, a->heap, &q);
				q->x = a->stack.n, q->y = dist;
				w->aux[0] = 1;
				ks_heapup_128y(a->heap.n, a->heap.a);
				kv_pushp(fm128_t, a->stack, &q);
				q->x = u, q->y = z.x;
				if (u == end) { // reach the end
					kv_pushp(fm128_t, a->rst, &q);
					q->x = a->stack.n - 1, q->y = dist;
				}
			}
		}
		if (i != r->n) break; // found 3 paths
	}
	// revert aux[]
	for (i = 0; i < a->stack.n; ++i) {
		p = &g->nodes.a[a->stack.a[i].x>>1];
		p->aux[0] = p->aux[1] = INT_MAX;
	}
	if (is_multi) return 0;
	// backtrace
	return a->walk.n;
}

int msg_peread(msg_t *g, int max_dist)
{
	hash64_t *h;
	fm128_v pairs;
	kv_init(pairs);
	h = build_hash(&g->nodes);
	collect_pairs(&g->nodes, h, &pairs);
	kh_destroy(64, h);
	free(pairs.a);
	return 0;
}
