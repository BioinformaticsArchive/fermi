#include <assert.h>
#include <stdio.h>
#include <pthread.h>
#include "fermi.h"
#include "rld.h"
#include "kvec.h"
#include "kstring.h"
#include "utils.h"

static uint64_t g_cnt, g_tot;
static int g_print_lock;

static inline void set_bit(uint64_t *bits, uint64_t x)
{
	uint64_t *p = bits + (x>>6);
	uint64_t z = 1LLU<<(x&0x3f), y;
	y = __sync_fetch_and_or(p, z);
	if ((y & z) == 0) {
		__sync_add_and_fetch(&g_cnt, 1);
	}
	__sync_add_and_fetch(&g_tot, 1);
}

static inline void set_bits(uint64_t *bits, const fmintv_t *p)
{
	uint64_t k;
	for (k = 0; k < p->x[2]; ++k) {
		set_bit(bits, p->x[0] + k);
		set_bit(bits, p->x[1] + k);
	}
}

// requirement: s[beg..l-1] must be a full read
static int unambi_nei_for(const rld_t *e, int min, int beg, kstring_t *s, fmintv_v *curr, fmintv_v *prev, uint64_t *bits, int first)
{
	extern fmintv_t fm6_overlap_intv(const rld_t *e, int len, const uint8_t *seq, int min, int j, int at5, fmintv_v *p);
	int i, j, c, old_l = s->l, ret;
	fmintv_t ik, ok[6];
	fmintv_v *swap;
	uint64_t w[6];

	curr->n = prev->n = 0;
	// backward search for overlapping reads
	ik = fm6_overlap_intv(e, s->l - beg, (uint8_t*)s->s + beg, min, s->l - beg - 1, 0, prev);
	//for (i = 0, c = 0; i < prev->n; ++i) c += prev->a[i].x[2]; fprintf(stderr, "Total: %d\n", c);
	if (prev->n > 0) {
		for (j = 0; j < prev->n; ++j) prev->a[j].info += beg;
		ret = prev->a[0].info; // the position with largest overlap
	} else ret = s->l - beg <= min? -1 : -6; // -1: too short; -6: no overlaps
	if (beg == 0 && first) { // test if s[beg..s->l-1] contained in another read
		fm6_extend(e, &ik, ok, 1); assert(ok[0].x[2]);
		if (ik.x[2] != ok[0].x[2]) ret = -2; // the sequence is left contained
		ik = ok[0];
		fm6_extend(e, &ik, ok, 0); assert(ok[0].x[2]);
		if (ik.x[2] != ok[0].x[2]) ret = -3; // the sequence is right contained
		set_bits(bits, ok); // mark the read(s) has been used
	}
	if (ret < 0) return ret;
	// forward search for the forward branching test
	for (;;) {
		int c0, n_c = 0;
		memset(w, 0, 48);
		for (j = 0, curr->n = 0; j < prev->n; ++j) {
			fmintv_t *p = &prev->a[j];
			fm6_extend(e, p, ok, 0);
			if (ok[0].x[2]) { // some reads end here
				if ((int32_t)p->info == ret && ok[0].x[2] == p->x[2]) {
					if (bits[ok[0].x[0]>>6]>>(ok[0].x[0]&0x3f)&1) ret = -10;
					set_bits(bits, ok);
					if (ret < 0) return ret;
					break;
				}
				set_bits(bits, ok); // mark the reads used
			}
			for (c = 1; c < 6; ++c)
				if (ok[c].x[2]) {
					if (w[c] == 0) ++n_c; // n_c keeps the number of non-zero elements in w[]
					w[c] += ok[c].x[2];
					ok[c].info = (p->info&0xffffffffU) | (uint64_t)c<<32;
					kv_push(fmintv_t, *curr, ok[c]);
				}
		}
		if (curr->n == 0) break; // cannot be extended
		if (j < prev->n) break; // found the only neighbor
		if (n_c > 1) {
			uint64_t max, sum;
			for (c0 = -1, max = sum = 0, c = 1; c < 6; ++c) {
				sum += w[c];
				if (w[c] > max) max = w[c], c0 = c;
			}
			if ((double)max / sum < 0.8 || sum - max > 2) return -7;
			for (i = j = 0; j < curr->n; ++j)
				if ((int)(curr->a[j].info>>32) == c0)
					curr->a[i++] = curr->a[j];
			curr->n = i;
		} else {
			for (c0 = 1; c0 < 6; ++c0)
				if (w[c0]) break;
		}
		kputc(fm6_comp(c0), s);
		swap = curr; curr = prev; prev = swap;
	}
	//for (i = 0; i < s->l; ++i) putchar("$ACGTN"[(int)s->s[i]]); putchar('\n');

	// forward search for reads overlapping the extension read from the 5'-end
	fm6_overlap_intv(e, s->l, (uint8_t*)s->s, min, ret, 1, prev);
	//printf("ret=%d, len=%d, prev->n=%d, %d\n", (int)ret, (int)s->l, (int)prev->n, min);
	// backward search for backward branching test
	for (i = ret - 1; i >= beg && prev->n; --i) {
		int c00 = s->s[i], c0, n_c = 0;
		memset(w, 0, 48);
		for (j = 0, curr->n = 0; j < prev->n; ++j) {
			fmintv_t *p = &prev->a[j];
			fm6_extend(e, p, ok, 1);
			if (ok[0].x[2]) set_bits(bits, ok);
			for (c = 1; c < 6; ++c)
				if (ok[c].x[2]) {
					if (w[c] == 0) ++n_c; // n_c keeps the number of non-zero elements in w[]
					w[c] += ok[c].x[2];
					ok[c].info = (p->info&0xffffffffU) | (uint64_t)c<<32;
					kv_push(fmintv_t, *curr, ok[c]);
				}
		}
		if (n_c > 1) {
			uint64_t max, sum;
			int i;
			for (c0 = -1, max = sum = 0, c = 1; c < 6; ++c) {
				sum += w[c];
				if (w[c] > max) max = w[c], c0 = c;
			}
			if ((double)max / sum < 0.8 || sum - max > 2 || c0 != c00) { // ambiguous; stop extension
				s->l = old_l;
				return c0 != c00? -9 : -8;
			}
			for (i = j = 0; j < curr->n; ++j)
				if ((int)(curr->a[j].info>>32) == c0)
					curr->a[i++] = curr->a[j];
			curr->n = i;
		}
		swap = curr; curr = prev; prev = swap;
	}
	//printf("final: ret=%d, len=%d\n", (int)ret, (int)s->l);
	return ret;
}

static void neighbor1(const rld_t *e, int min, uint64_t start, uint64_t step, uint64_t *bits, FILE *fp)
{
	extern void seq_reverse(int l, unsigned char *s);
	extern void seq_revcomp6(int l, unsigned char *s);
	fmintv_v a[2];
	kstring_t s, out;
	uint64_t x, k;

	kv_init(a[0]); kv_init(a[1]);
	s.l = s.m = 0; s.s = 0;
	out.l = out.m = 0; out.s = 0;
	for (x = start<<1|1; x < e->mcnt[1]; x += step<<1) {
		//if (x == 7) break;
		int i, beg = 0, ori_len, ret1;
		k = fm_retrieve(e, x, &s);
		if (bits[k>>6]>>(k&0x3f)&1) continue; // the read has been used
		ori_len = s.l;
		seq_reverse(s.l, (uint8_t*)s.s);
		while ((beg = unambi_nei_for(e, min, beg, &s, &a[0], &a[1], bits, 1)) >= 0);
		if ((ret1 = beg) <= -6) { // stop due to branching or no overlaps
			beg = s.l - ori_len;
			seq_revcomp6(s.l, (uint8_t*)s.s);
			while ((beg = unambi_nei_for(e, min, beg, &s, &a[0], &a[1], bits, 0)) >= 0);
		}
		kputc('>', &out); kputl((long)x, &out); kputc(' ', &out); kputw(ret1, &out); kputw(beg, &out); kputc('\n', &out);
		for (i = 0; i < s.l; ++i)
			kputc("$ACGTN"[(int)s.s[i]], &out);
		kputc('\n', &out);
		if (__sync_bool_compare_and_swap(&g_print_lock, 0, 1)) {
			fputs(out.s, fp);
			out.l = 0; out.s[0] = 0;
			__sync_bool_compare_and_swap(&g_print_lock, 1, 0);
		}
	}
	while (__sync_bool_compare_and_swap(&g_print_lock, 0, 1) == 0); // busy waiting
	fputs(out.s, fp);
	__sync_bool_compare_and_swap(&g_print_lock, 1, 0);
	free(a[0].a); free(a[1].a);
	free(s.s); free(out.s);
}

typedef struct {
	int min;
	uint64_t start, step, *bits;
	const rld_t *e;
} worker_t;

static void *worker(void *data)
{
	worker_t *w = (worker_t*)data;
	neighbor1(w->e, w->min, w->start, w->step, w->bits, stdout);
	return 0;
}

int fm6_unambi_join(const rld_t *e, int min, int n_threads)
{
	uint64_t *bits;
	pthread_t *tid;
	pthread_attr_t attr;
	worker_t *w;
	int j;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	w = (worker_t*)calloc(n_threads, sizeof(worker_t));
	tid = (pthread_t*)calloc(n_threads, sizeof(pthread_t));
	bits = (uint64_t*)xcalloc((e->mcnt[1] + 63)/64, 8);
	for (j = 0; j < n_threads; ++j) {
		worker_t *ww = w + j;
		ww->e = e;
		ww->min = min;
		ww->step = n_threads;
		ww->start = j;
		ww->bits = bits;
	}
	for (j = 0; j < n_threads; ++j) pthread_create(&tid[j], &attr, worker, w + j);
	for (j = 0; j < n_threads; ++j) pthread_join(tid[j], 0);
	free(w); free(tid); free(bits);
//	fprintf(stderr, "%lld, %lld\n", g_cnt, g_tot);
	return 0;
}