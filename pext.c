#include <zlib.h>
#include <ctype.h>
#include <stdlib.h>
#include <assert.h>
#include "rld.h"
#include "fermi.h"
#include "kstring.h"
#include "kvec.h"
#include "kseq.h"
KSEQ_DECLARE(gzFile)

#define BUF_SIZE   0x10000
#define MIN_INSERT 50

extern unsigned char seq_nt6_table[128];

typedef struct {
	int len;
	uint8_t *semitig;
	fm64_v reads;
} ext1_t;

static int read_unitigs(kseq_t *kseq, int n, ext1_t *buf, int min_dist, int max_dist)
{
	int k, j = 0;
	fm128_v reads;
	assert(n >= 3);
	kv_init(reads);
	while (kseq_read(kseq) >= 0) {
		char *q;
		ext1_t *p;
		int beg, end;
		if (kseq->comment.l == 0) continue; // no comments
		if (kseq->seq.l < min_dist) continue; // too short; skip
		reads.n = 0;
		for (k = 0, q = kseq->comment.s; *q && k < 2; ++q) // skip the first two fields
			if (isspace(*q)) ++k;
		while (isdigit(*q)) { // read mapping
			fm128_t x;
			x.x = strtol(q, &q, 10); ++q;
			x.y = *q == '-'? 1 : 0; q += 2;
			x.y |= (uint64_t)strtol(q, &q, 10)<<32; ++q;
			x.y |= strtol(q, &q, 10)<<1;
			kv_push(fm128_t, reads, x);
			if (*q++ == 0) break;
		}
		// left-end
		p = &buf[j++];
		p->reads.n = 0;
		end = kseq->seq.l < max_dist? kseq->seq.l : max_dist;
		for (k = 0; k < reads.n; ++k) {
			fm128_t *r = &reads.a[k];
			if ((r->y&1) && r->y<<32>>33 <= end)
				kv_push(uint64_t, p->reads, (r->x^1)<<1);
		}
		if (p->reads.n) { // potentially extensible
			p->len = end;
			p->semitig = calloc(p->len + 1, 1);
			for (k = 0; k < end; ++k)
				p->semitig[k] = seq_nt6_table[(int)kseq->seq.s[k]];
		} else --j;
		// right-end
		p = &buf[j++];
		p->reads.n = 0;
		beg = kseq->seq.l < max_dist? 0 : kseq->seq.l - max_dist;
		for (k = 0; k < reads.n; ++k) {
			fm128_t *r = &reads.a[k];
			if ((r->y&1) == 0 && r->y>>32 >= beg)
				kv_push(uint64_t, p->reads, (r->x^1)<<1|1);
		}
		if (p->reads.n) {
			p->len = kseq->seq.l - beg;
			p->semitig = calloc(p->len + 1, 1);
			for (k = 0; k < p->len; ++k)
				p->semitig[k] = seq_nt6_table[(int)kseq->seq.s[k + beg]];
		}
		if (j + 1 >= n) break;
	}
	return j;
}

static void pext_core(const rld_t *e, int n, ext1_t *buf, int start, int step)
{
	extern void seq_reverse(int l, unsigned char *s);
	kstring_t rd, seq;
	int i, j;
	rd.l = seq.l = rd.m = seq.m = 0; rd.s = seq.s = 0;
	for (i = start; i < n; i += step) {
		ext1_t *p = &buf[i];
		msg_t *g;
		int tmp;
		seq.l = 0;
		kputsn((char*)p->semitig, p->len + 1, &seq); // +1 to include the ending NULL
		for (j = 0; j < p->reads.n; ++j) {
			assert(p->reads.a[j] < e->mcnt[1]);
			fm_retrieve(e, p->reads.a[j], &rd);
			seq_reverse(rd.l, (uint8_t*)rd.s);
			kputsn(rd.s, rd.l + 1, &seq);
		}
		tmp = fm_verbose; fm_verbose = 1;
		g = fm6_api_unitig();
		//printf("=== %d ===\n", i);
		if (i == 1) {
		for (j = 0; j < seq.l; ++j) {
			if (seq.s[j] == 0) putchar('\n');
			else putchar("$ACGTN"[(int)seq.s[j]]);
		}
		}
	}
	free(rd.s); free(seq.s);
}

int fm6_pext(const rld_t *e, const char *fng, int min_ovlp, int n_threads, double avg, double std)
{
	int min_dist, max_dist;
	kseq_t *kseq;
	gzFile fp;
	ext1_t *buf;
	int i, n;

	max_dist = (int)(avg + std * 2. + .499);
	min_dist = (int)(avg - std * 2. + .499);
	if (min_dist < MIN_INSERT) min_dist = MIN_INSERT;
	fp = gzopen(fng, "rb");
	if (fp == 0) return -1;
	kseq = kseq_init(fp);
	buf = calloc(BUF_SIZE, sizeof(ext1_t));

	while ((n = read_unitigs(kseq, BUF_SIZE, buf, min_dist, max_dist)) > 0) {
		pext_core(e, n, buf, 0, 1);
	}

	for (i = 0; i < BUF_SIZE; ++i) {
		free(buf[i].semitig);
		free(buf[i].reads.a);
	}
	free(buf);
	kseq_destroy(kseq);
	gzclose(fp);
	return 0;
}
