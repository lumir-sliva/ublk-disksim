// SPDX-License-Identifier: GPL-2.0
/*
 * ssd: timing model of a flash SSD (SATA or NVMe), on top of a backing
 * device (normally RAM).
 *
 * Data goes to the backing device immediately; the model only decides when
 * each request completes. What it models:
 *
 *  - `dies` flash dies, each with its own timeline. A read of a page
 *    (die = page number mod dies) occupies its die for tr_us plus the
 *    transfer over the flash channel. With tr_step_us each 4K of data
 *    sits on one of the three TLC page types (lower, middle, upper,
 *    by a hash of its address, a third each), sensed with 1, 2 and 4
 *    read levels: tr_us, + tr_step_us, + 3 tr_step_us (a read takes
 *    its slowest 4K). A read waits for whatever its die is doing, a
 *    program included, unless susp_us is set: then a read that finds
 *    the die programming starts susp_us later, at the program's next
 *    suspend point (reads arriving meanwhile join it), and the program
 *    resumes after the reads, later by their die time. A program still
 *    receiving its page over the channel hasn't started and can't be
 *    suspended: a read arriving then waits for all of it. Flushes
 *    wait for the resumed program; buffer space is freed at the page's
 *    unsuspended end. Channels are not a shared resource: their total
 *    rate is above the host link on the drives this was fitted to;
 *  - write history: each page_kb chunk of the device remembers whether
 *    it was last written in one piece (a write or a sequential stream
 *    that covered it) or by random writes, which scatter its 4K pieces
 *    over the flash. A read of a scattered chunk is one page read per
 *    4K, each on its own die; `history` 1 starts with every chunk
 *    scattered (a drive preconditioned with random writes), 0 (default)
 *    with all written in one piece;
 *  - one host link: each command occupies it for cmd_us plus its data at
 *    iface_mbps (reads after the flash read, writes on arrival), then
 *    iface_us of controller latency until completion;
 *  - a write buffer (buf_mb): writes complete once their data is in it.
 *    Full pages (page_kb) are programmed on whichever die is free first,
 *    as a log-structured drive does. A page of random writes costs `waf`
 *    program units instead of one: steady-state garbage collection folded
 *    into the page that caused it. Writes continuing one of the last 8
 *    write streams count as sequential and cost 1 + (waf - 1) x the
 *    scattered share of the whole device: freeing space on a drive whose
 *    data random writes have scattered needs copies whatever the new data
 *    is (on a fresh, sequentially written drive they cost one unit). A
 *    full buffer makes writes wait. With gc_mbps and gc_pool_mb, garbage
 *    collection also works ahead: while no page waits to be programmed
 *    it prepares erased space at gc_mbps, up to gc_pool_mb, and pages
 *    that need copies use that first at one unit per page (the refill
 *    takes no die time: a simplification);
 *  - FLUSH: nothing is advertised with vwc = 0, so the kernel sends none.
 *    With plp = 1 the buffer is durable and a flush costs flush_us after
 *    it is issued; with plp = 0 the partly filled page is closed and the
 *    flush waits until every buffered page is programmed, plus flush_us
 *    (mapping-table commit). A flush with nothing written since the
 *    previous one is free. On SATA it is non-queued, as FLUSH CACHE:
 *    earlier commands finish first, everything arriving meanwhile, reads
 *    included, waits for it. On NVMe it is queued and holds nothing up;
 *  - floor_us: the host's own overhead per request (ublk round trip and
 *    the server waking for its timer), subtracted from each completion
 *    (never below the request's arrival), so the parameters stay device
 *    latencies.
 *
 * Not modelled: garbage collection as a process (idle-time GC, fill level,
 * over-provisioning; on a full drive sequential writes pay for it too),
 * SLC caching, erase suspend, a limit on suspends per program, reads
 * served from the write buffer, mapping-table cache misses, TRIM,
 * thermal throttling, multiple NVMe queues. Calibrate against the
 * drive you want to imitate before trusting absolute numbers.
 *
 * No clock and no I/O of its own: time, completions and wake-ups go
 * through struct model_env (model.h). The kublk target is ssd.c.
 */

#include "ssd_model.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __u64 max_u64(__u64 a, __u64 b)
{
	return a > b ? a : b;
}

static __u64 min_u64(__u64 a, __u64 b)
{
	return a < b ? a : b;
}

static __u64 xfer_ns(double mbps, __u64 bytes)
{
	return (__u64)((double)bytes / (mbps * 1e6) * 1e9);
}

/* splitmix64 finalizer: a fixed, well-spread hash of a 4K index */
static __u64 mix64(__u64 k)
{
	k *= 0x9e3779b97f4a7c15ULL;
	k = (k ^ (k >> 30)) * 0xbf58476d1ce4e5b9ULL;
	k = (k ^ (k >> 27)) * 0x94d049bb133111ebULL;
	return k ^ (k >> 31);
}

/* write history of chunk c: 1 = scattered by random writes */
static int hist_get(const struct ssd_model *m, __u64 c)
{
	return c < m->nchunk && (m->hist[c >> 3] >> (c & 7) & 1);
}

static void hist_set(struct ssd_model *m, __u64 c, int v)
{
	if (c >= m->nchunk || hist_get(m, c) == v)
		return;
	if (v) {
		m->hist[c >> 3] |= 1 << (c & 7);
		m->nscat++;
	} else {
		m->hist[c >> 3] &= ~(1 << (c & 7));
		m->nscat--;
	}
}

/* complete request r at model time `when`, less the host floor */
static void done(struct ssd_model *m, const struct ssd_req *r, __u64 when)
{
	__u64 at = when > r->arrive + m->floor_ns ? when - m->floor_ns :
		r->arrive;

	m->last_done = max_u64(m->last_done, when);
	m->env.done(m->env.ctx, r->tag, at);
}

static void flush_done(struct ssd_model *m, const struct ssd_req *r,
		       __u64 when)
{
	__u64 d = when - r->arrive;

	m->flush_ns_sum += d;
	if (d > m->flush_ns_max)
		m->flush_ns_max = d;
	done(m, r, when);
}

/* earliest start >= t of a free stretch of `len` on the host link */
static __u64 link_reserve(struct ssd_model *m, __u64 t, __u64 len)
{
	__u64 s = t;
	int i;

	for (i = 0; i < m->nlink; i++) {
		if (m->link[i].end <= s)
			continue;
		if (m->link[i].start >= s + len)
			break;
		s = m->link[i].end;
	}
	if (m->nlink == m->cap_link) {
		m->cap_link *= 2;
		m->link = realloc(m->link, m->cap_link * sizeof(*m->link));
		assert(m->link);
	}
	memmove(m->link + i + 1, m->link + i, (m->nlink - i) * sizeof(*m->link));
	m->link[i].start = s;
	m->link[i].end = s + len;
	m->nlink++;
	return s + len;
}

static void link_prune(struct ssd_model *m)
{
	int n = 0;

	while (n < m->nlink && m->link[n].end <= m->now)
		n++;
	if (n) {
		memmove(m->link, m->link + n, (m->nlink - n) * sizeof(*m->link));
		m->nlink -= n;
	}
}

static struct ssd_page *page(struct ssd_model *m, __u64 seq)
{
	return &m->pg[seq & (m->cap_pg - 1)];
}

static void heap_push(struct ssd_model *m, __u64 seq)
{
	__u64 i = m->nheap++;

	while (i) {
		__u64 up = (i - 1) / 2;

		if (page(m, m->heap[up])->end <= page(m, seq)->end)
			break;
		m->heap[i] = m->heap[up];
		i = up;
	}
	m->heap[i] = seq;
}

static __u64 heap_pop(struct ssd_model *m)
{
	__u64 top = m->heap[0], last = m->heap[--m->nheap], i = 0;

	for (;;) {
		__u64 c = 2 * i + 1;

		if (c >= m->nheap)
			break;
		if (c + 1 < m->nheap && page(m, m->heap[c + 1])->end <
		    page(m, m->heap[c])->end)
			c++;
		if (page(m, last)->end <= page(m, m->heap[c])->end)
			break;
		m->heap[i] = m->heap[c];
		i = c;
	}
	if (m->nheap)
		m->heap[i] = last;
	return top;
}

/*
 * Bring the pool up to time t: garbage collection refills it at gc_mbps
 * while no page is waiting to be programmed and the dies have finished
 * the last one.
 */
static void pool_refill(struct ssd_model *m, __u64 t)
{
	__u64 from = max_u64(m->max_end_started, m->pool_t);

	if (m->pool_cap > 0 && m->seq_start == m->seq_next && t > from) {
		m->pool += m->p.gc_mbps * 1e6 * (double)(t - from) / 1e9;
		if (m->pool > m->pool_cap)
			m->pool = m->pool_cap;
	}
	m->pool_t = max_u64(m->pool_t, t);
}

/* the open page becomes a closed page, ready to program at `ready` */
static void close_open(struct ssd_model *m, __u64 ready)
{
	struct ssd_page *pg;
	double gc, used;

	if (!m->open_bytes)
		return;
	if (m->seq_next - m->seq_head == m->cap_pg) {
		__u64 cap = m->cap_pg * 2, s;
		struct ssd_page *n = calloc(cap, sizeof(*n));

		assert(n);
		for (s = m->seq_head; s < m->seq_next; s++)
			n[s & (cap - 1)] = *page(m, s);
		free(m->pg);
		m->pg = n;
		m->cap_pg = cap;
		m->heap = realloc(m->heap, cap * sizeof(*m->heap));
		assert(m->heap);
	}
	/* bytes that need GC copies take erased space from the pool first */
	pool_refill(m, ready);
	gc = m->open_gc;
	used = gc < m->pool ? gc : m->pool;
	m->pool -= used;
	m->pool_used += used;
	pg = page(m, m->seq_next++);
	pg->bytes = m->open_bytes;
	pg->ready = ready;
	pg->units = 1 + (m->p.waf - 1) * (gc - used) / m->open_bytes;
	pg->end = 0;
	pg->freed = 0;
	m->open_bytes = m->open_gc = 0;
}

/*
 * Sequential if it continues one of the recent write streams (the
 * longest, if several end here). *run is where the write's contiguous
 * run began: the stream's start, or its own lba for a new stream.
 */
static int classify(struct ssd_model *m, __u64 lba, __u64 nr, __u64 *run)
{
	int i, lru = 0, hit = -1;

	m->stream_clock++;
	for (i = 0; i < SSD_NSTREAMS; i++) {
		if (m->stream_end[i] == lba && m->stream_use[i] &&
		    (hit < 0 || m->stream_start[i] < m->stream_start[hit]))
			hit = i;
		if (m->stream_use[i] < m->stream_use[lru])
			lru = i;
	}
	if (hit >= 0) {
		m->stream_end[hit] = lba + nr;
		m->stream_use[hit] = m->stream_clock;
		*run = m->stream_start[hit];
		return 0;
	}
	m->stream_end[lru] = lba + nr;
	m->stream_start[lru] = lba;
	m->stream_use[lru] = m->stream_clock;
	*run = lba;
	return 1;
}

/*
 * Record how [lba, lba + nr) was written. A chunk that ends inside the
 * write and lies wholly within its contiguous run from `run` was written
 * in one piece; a chunk a random write covers only partly is scattered;
 * a stream's partly written chunk keeps its state until the write that
 * completes it. Returns the bytes whose space garbage collection has to
 * free by copying: all of a random write; of a sequential one, the share
 * of the device that is scattered (GC takes its victims from the whole
 * drive, so it's the drive's state that counts, not the data overwritten).
 */
static __u64 mark_history(struct ssd_model *m, __u64 lba, __u64 nr,
			  __u64 run, int rnd)
{
	__u64 off = lba << 9, end = off + (nr << 9), pb = m->page_bytes, c;
	__u64 gc = rnd ? nr << 9 :
		(__u64)((double)(nr << 9) * m->nscat / m->nchunk);

	for (c = off / pb; c * pb < end; c++) {
		if (c * pb >= run << 9 && (c + 1) * pb <= end)
			hist_set(m, c, 0);
		else if (rnd)
			hist_set(m, c, 1);
	}
	return gc;
}

/* write data enters the buffer at `a`; the write completes */
static void admit(struct ssd_model *m, const struct ssd_req *r, __u64 a)
{
	__u64 bytes = r->nr << 9, left = bytes;

	m->buf_bytes += left;
	m->dirty = 1;
	while (left) {
		__u64 take = min_u64(left, m->page_bytes - m->open_bytes);

		m->open_bytes += take;
		m->open_gc += take * r->gc_bytes / bytes;
		m->open_last = max_u64(m->open_last, a);
		left -= take;
		if (m->open_bytes == m->page_bytes)
			close_open(m, m->open_last);
	}
	done(m, r, a + m->iface_ns);
}

static void start(struct ssd_model *m, struct ssd_req r);

/* the pending SATA flush is done at `when`: release what it held */
static void sata_flush_finish(struct ssd_model *m, __u64 when)
{
	struct ssd_req *held = m->held;
	int i, n = m->nheld;

	flush_done(m, &m->sflush, when);
	m->sflush_tag = -1;
	m->block_until = max_u64(m->block_until, when);

	/* replay; anything a replayed flush holds again goes to the spare */
	m->held = m->spare;
	m->spare = held;
	m->nheld = 0;
	for (i = 0; i < n; i++)
		start(m, held[i]);
}

/*
 * Start program units up to `now` on the die free first. Pages start in
 * order; when a page's last unit has started, NVMe flushes waiting for
 * it complete.
 */
static int run_units(struct ssd_model *m)
{
	int progress = 0;

	m->next_unit = ~0ULL;
	while (m->seq_start < m->seq_next) {
		struct ssd_page *pg = page(m, m->seq_start);
		unsigned d, best = 0;
		__u64 s;
		double u;

		for (d = 1; d < m->p.dies; d++)
			if (m->die_free[d] < m->die_free[best])
				best = d;
		s = max_u64(m->die_free[best], pg->ready);
		if (s > m->now) {
			m->next_unit = s;
			break;
		}
		u = pg->units < 1 ? pg->units : 1;
		m->die_free[best] = s + (__u64)(u * m->unit_ns);
		m->prog_end[best] = m->die_free[best];
		m->prog_from[best] = s + (__u64)(u * m->page_xfer_ns);
		pg->end = max_u64(pg->end, m->die_free[best]);
		pg->units -= u;
		m->units_done += u;
		progress = 1;
		if (pg->units > 1e-9)
			continue;

		heap_push(m, m->seq_start);
		m->max_end_started = max_u64(m->max_end_started, pg->end);
		while (m->nnflush && m->nflush[0].seq <= m->seq_start) {
			struct ssd_flush f = m->nflush[0];

			memmove(m->nflush, m->nflush + 1,
				--m->nnflush * sizeof(*m->nflush));
			flush_done(m, &f.r, max_u64(f.r.t, m->max_end_started) +
				   m->flush_ns);
		}
		m->seq_start++;
	}
	return progress;
}

/* free programmed pages in order of their end; admit waiting writes */
static int free_pages(struct ssd_model *m)
{
	int progress = 0;

	while (m->nheap && page(m, m->heap[0])->end <= m->now) {
		struct ssd_page *pg = page(m, heap_pop(m));
		__u64 at = pg->end;

		pg->freed = 1;
		m->buf_bytes -= pg->bytes;
		progress = 1;
		while (m->nwait && m->buf_bytes + (m->wait[0].nr << 9) <=
		       m->buf_cap) {
			struct ssd_req w = m->wait[0];

			memmove(m->wait, m->wait + 1, --m->nwait * sizeof(*m->wait));
			admit(m, &w, max_u64(at, w.link_end));
		}
	}
	while (m->seq_head < m->seq_start && page(m, m->seq_head)->freed)
		m->seq_head++;
	return progress;
}

/* the pending SATA flush, once every earlier command has been taken */
static int sata_flush_check(struct ssd_model *m)
{
	__u64 drain;

	if (m->sflush_tag < 0 || m->nwait)
		return 0;
	drain = max_u64(m->sflush.t, m->last_done);
	if (!m->sflush_closed) {
		m->sflush_closed = 1;
		if (!m->dirty) {	/* nothing written since: free, as hdd */
			sata_flush_finish(m, drain);
			return 1;
		}
		m->dirty = 0;
		if (!m->p.plp) {
			/*
			 * The drive programs a partial page only because of the
			 * flush, so not before it; and it's a new page to
			 * program: let run_units see it.
			 */
			close_open(m, max_u64(m->open_last, m->sflush.t));
			m->sflush_seq = m->seq_next;
			return 1;
		}
	}
	if (!m->p.plp) {
		if (m->seq_start < m->sflush_seq)
			return 0;
		drain = max_u64(drain, m->max_end_started);
	}
	sata_flush_finish(m, drain + m->flush_ns);
	return 1;
}

/* catch the model up to now and ask for a wake-up at its next event */
static void pump(struct ssd_model *m)
{
	__u64 next;

	m->now = m->env.now(m->env.ctx);
	link_prune(m);
	while (run_units(m) | free_pages(m) | sata_flush_check(m))
		;
	next = m->next_unit;
	if (m->nwait && m->nheap)
		next = min_u64(next, page(m, m->heap[0])->end);
	if (next != ~0ULL)
		m->env.wake(m->env.ctx, next);
}

/* a read of `len` die time reaches die d at t: when it starts on the die */
static __u64 die_read(struct ssd_model *m, unsigned d, __u64 t, __u64 len)
{
	__u64 s, *pe = &m->prog_end[d], *re = &m->read_end[d];

	/*
	 * a program running past its data transfer at t, and no read
	 * queued behind it
	 */
	if (m->susp_ns && *pe > t && *re < *pe && t >= m->prog_from[d]) {
		if (*re > t)			/* suspended: join the reads */
			s = *re;
		else if (t + m->susp_ns >= *pe)	/* done before it could suspend */
			s = *pe;
		else
			s = t + m->susp_ns;
		if (s < *pe) {
			*pe += len;
			/* flushes without PLP wait for the resumed program */
			m->max_end_started = max_u64(m->max_end_started, *pe);
			m->n_read_susp++;
		}
	} else {
		s = max_u64(m->die_free[d], t);
	}
	*re = s + len;
	m->die_free[d] = max_u64(*pe, *re);
	return s;
}

/* extra sense time of the slowest 4K in [s0, e0): TLC page types 1-2-4 */
static __u64 read_levels_ns(struct ssd_model *m, __u64 s0, __u64 e0)
{
	static const unsigned extra[3] = { 0, 1, 3 };
	unsigned most = 0;
	__u64 k;

	if (!m->tr_step_ns)
		return 0;
	for (k = s0 >> 12; k << 12 < e0 && most < 3; k++) {
		__u64 h = mix64(k);

		if (extra[h % 3] > most)
			most = extra[h % 3];
	}
	return most * m->tr_step_ns;
}

/* one page read of `len` die time on die d, for a read arriving at t */
static void read_piece(struct ssd_model *m, unsigned d, __u64 t, __u64 len,
		       __u64 *wait, __u64 *fdone)
{
	__u64 s = die_read(m, d, t, len);

	if (s - t > *wait)
		*wait = s - t;
	*fdone = max_u64(*fdone, s + len);
}

static void do_read(struct ssd_model *m, const struct ssd_req *r)
{
	__u64 off = r->lba << 9, end = off + (r->nr << 9), fdone = r->t, p;
	__u64 wait = 0;

	m->n_read++;
	for (p = off / m->page_bytes; p * m->page_bytes < end; p++) {
		__u64 s0 = max_u64(p * m->page_bytes, off);
		__u64 e0 = min_u64((p + 1) * m->page_bytes, end);
		__u64 k;

		if (!hist_get(m, p)) {
			read_piece(m, p % m->p.dies, r->t, m->tr_ns +
				   read_levels_ns(m, s0, e0) +
				   xfer_ns(m->p.ch_mbps, e0 - s0), &wait, &fdone);
			continue;
		}
		/* scattered: each 4K is a page read of its own, on its own die */
		for (k = s0 >> 12; k << 12 < e0; k++) {
			__u64 a = max_u64(k << 12, s0), b = min_u64((k + 1) << 12, e0);

			read_piece(m, (mix64(k) >> 32) % m->p.dies, r->t, m->tr_ns +
				   read_levels_ns(m, a, b) +
				   xfer_ns(m->p.ch_mbps, b - a), &wait, &fdone);
			m->n_frag_reads++;
		}
	}
	if (wait) {
		m->n_read_wait++;
		m->read_wait_ns_sum += wait;
	}
	done(m, r, link_reserve(m, fdone, m->cmd_ns +
				xfer_ns(m->p.iface_mbps, r->nr << 9)) +
	     m->iface_ns);
}

static void do_write(struct ssd_model *m, struct ssd_req r)
{
	__u64 bytes = r.nr << 9, run;

	m->n_write++;
	r.link_end = link_reserve(m, r.t, m->cmd_ns +
				  xfer_ns(m->p.iface_mbps, bytes));
	r.rnd = classify(m, r.lba, r.nr, &run);
	r.gc_bytes = mark_history(m, r.lba, r.nr, run, r.rnd);
	if (r.rnd)
		m->rnd_bytes += bytes;
	else
		m->seq_bytes += bytes;
	if (!m->nwait && m->buf_bytes + bytes <= m->buf_cap) {
		admit(m, &r, r.link_end);
	} else {
		m->n_buf_full++;
		m->wait[m->nwait++] = r;
	}
}

static void do_flush(struct ssd_model *m, const struct ssd_req *r)
{
	if (!m->p.vwc) {	/* nothing advertised, nothing to do */
		done(m, r, r->t);
		return;
	}
	m->n_flush++;
	if (!m->p.nvme) {
		m->sflush_tag = r->tag;
		m->sflush = *r;
		m->sflush_closed = 0;
		return;
	}
	if (!m->dirty) {	/* nothing written since the last flush */
		flush_done(m, r, r->t);
		return;
	}
	m->dirty = 0;
	if (m->p.plp) {
		flush_done(m, r, r->t + m->flush_ns);
		return;
	}
	close_open(m, max_u64(m->open_last, r->t));
	if (m->seq_start == m->seq_next) {
		flush_done(m, r, max_u64(r->t, m->max_end_started) + m->flush_ns);
		return;
	}
	m->nflush[m->nnflush].r = *r;
	m->nflush[m->nnflush].seq = m->seq_next - 1;
	m->nnflush++;
}

/* a request reaches the model at r.t (or is released by a SATA flush) */
static void start(struct ssd_model *m, struct ssd_req r)
{
	if (m->sflush_tag >= 0) {
		m->held[m->nheld++] = r;
		return;
	}
	if (r.t < m->block_until) {	/* behind a SATA flush */
		m->n_blocked++;
		m->blocked_ns_sum += m->block_until - r.t;
		r.t = m->block_until;
	}
	switch (r.op) {
	case MODEL_READ:
		do_read(m, &r);
		break;
	case MODEL_WRITE:
		do_write(m, r);
		break;
	case MODEL_FLUSH:
		do_flush(m, &r);
		break;
	default:
		assert(0);
	}
}

/* ---- interface ---------------------------------------------------- */

/*
 * Profiles: named parameter sets fitted to spec sheets and published
 * sync-write measurements; see README for the sources and the
 * calibration status of each number.
 */
static const struct {
	const char *name;
	struct ssd_params p;
} profiles[] = {
	/* Samsung PM883 960 GB: enterprise SATA, power-loss protection */
	{ "sata-plp", {
		.nvme = 0, .iface_mbps = 560, .cmd_us = 3, .iface_us = 29,
		.dies = 32, .page_kb = 16, .tr_us = 75, .ch_mbps = 800,
		.tprog_us = 700, .waf = 7.3, .buf_mb = 32,
		.plp = 1, .vwc = 1, .flush_us = 15,
	} },
	/* Samsung PM9A3 1.92 TB: enterprise NVMe (PCIe 4 x4), PLP, no VWC */
	{ "nvme-plp", {
		.nvme = 1, .iface_mbps = 6900, .cmd_us = 0, .iface_us = 14,
		.dies = 64, .page_kb = 16, .tr_us = 62, .ch_mbps = 1200,
		.tprog_us = 400, .waf = 4.9, .buf_mb = 64,
		.plp = 1, .vwc = 0, .flush_us = 0,
	} },
	/* Samsung 870 EVO 1 TB: consumer SATA, no power-loss protection */
	{ "sata-consumer", {
		.nvme = 0, .iface_mbps = 560, .cmd_us = 3, .iface_us = 17.5,
		.dies = 16, .page_kb = 16, .tr_us = 46, .ch_mbps = 1200,
		.tprog_us = 390, .waf = 13.7, .buf_mb = 32,
		.plp = 0, .vwc = 1, .flush_us = 3200,
	} },
	/*
	 * Micron 7300 PRO 3.84 TB M.2: enterprise NVMe (PCIe 3 x4), PLP,
	 * no VWC; 64 dies of 512 Gb 96-layer TLC. Structure measured on the
	 * drive (see VALIDATION): TLC page types a third each (mean read =
	 * the datasheet's), 64 KiB programs of 2.7 ms (the datasheet's
	 * sequential write rate), reads suspend programs. cmd_us caps 4K
	 * reads at 1 / (cmd + 4K link) = 519K, the datasheet's 520K at
	 * QD512 (the drive: 534K at QD256).
	 */
	{ "micron-7300", {
		.nvme = 1, .iface_mbps = 3000, .cmd_us = 0.56, .iface_us = 23.6,
		.dies = 64, .page_kb = 64, .tr_us = 25, .tr_step_us = 26,
		.ch_mbps = 800, .tprog_us = 2624, .waf = 5.0, .susp_us = 20,
		.buf_mb = 64, .plp = 1, .vwc = 0, .flush_us = 0,
	} },
};

const struct ssd_params *ssd_profile(const char *name)
{
	unsigned i;

	if (!name)
		return &profiles[0].p;
	for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++)
		if (!strcmp(profiles[i].name, name))
			return &profiles[i].p;
	return NULL;
}

int ssd_params_check(const struct ssd_params *p, __u64 max_io_bytes,
		     unsigned depth)
{
	if (p->dies == 0 || p->page_kb < 4 || p->tr_us <= 0 ||
	    p->tprog_us <= 0 || p->ch_mbps <= 0 || p->iface_mbps <= 0 ||
	    p->waf < 1 || p->cmd_us < 0 || p->iface_us < 0 ||
	    p->flush_us < 0 || p->floor_us < 0 || p->susp_us < 0 ||
	    p->tr_step_us < 0 || p->history > 1 || p->gc_pool_mb < 0 ||
	    p->gc_mbps < 0 ||
	    ((__u64)p->buf_mb << 20) < max_io_bytes +
	    ((__u64)p->page_kb << 10) ||
	    (!p->nvme && depth > 32))
		return -EINVAL;
	return 0;
}

struct ssd_model *ssd_model_new(const struct ssd_params *p, __u64 sectors,
				unsigned depth, const struct model_env *env)
{
	struct ssd_model *m = calloc(1, sizeof(*m));
	__u64 hist_bytes;

	assert(m);
	m->p = *p;
	m->env = *env;
	m->depth = depth;
	m->page_bytes = (__u64)p->page_kb << 10;
	m->buf_cap = (__u64)p->buf_mb << 20;
	m->nchunk = ((sectors << 9) + m->page_bytes - 1) / m->page_bytes;
	hist_bytes = (m->nchunk + 7) / 8;
	m->hist = calloc(hist_bytes ? hist_bytes : 1, 1);
	assert(m->hist);
	if (p->history) {
		memset(m->hist, 0xff, hist_bytes);
		m->nscat = m->nchunk;
	}
	m->tr_ns = (__u64)(p->tr_us * 1e3);
	m->tr_step_ns = (__u64)(p->tr_step_us * 1e3);
	m->cmd_ns = (__u64)(p->cmd_us * 1e3);
	m->iface_ns = (__u64)(p->iface_us * 1e3);
	m->flush_ns = (__u64)(p->flush_us * 1e3);
	m->susp_ns = (__u64)(p->susp_us * 1e3);
	m->floor_ns = (__u64)(p->floor_us * 1e3);
	m->page_xfer_ns = xfer_ns(p->ch_mbps, m->page_bytes);
	m->unit_ns = m->page_xfer_ns + (__u64)(p->tprog_us * 1e3);
	m->die_free = calloc(p->dies, sizeof(*m->die_free));
	m->prog_end = calloc(p->dies, sizeof(*m->prog_end));
	m->read_end = calloc(p->dies, sizeof(*m->read_end));
	m->prog_from = calloc(p->dies, sizeof(*m->prog_from));
	m->cap_link = 2 * depth;
	m->link = calloc(m->cap_link, sizeof(*m->link));
	m->cap_pg = 1024;
	m->pg = calloc(m->cap_pg, sizeof(*m->pg));
	m->heap = calloc(m->cap_pg, sizeof(*m->heap));
	m->wait = calloc(depth, sizeof(*m->wait));
	m->held = calloc(depth, sizeof(*m->held));
	m->spare = calloc(depth, sizeof(*m->spare));
	m->nflush = calloc(depth, sizeof(*m->nflush));
	assert(m->die_free && m->prog_end && m->read_end && m->prog_from &&
	       m->link && m->pg && m->heap && m->wait && m->held && m->spare &&
	       m->nflush);
	m->sflush_tag = -1;
	m->pool_cap = p->gc_mbps > 0 ? p->gc_pool_mb * 1048576.0 : 0;
	return m;
}

void ssd_model_free(struct ssd_model *m)
{
	if (!m)
		return;
	free(m->die_free);
	free(m->prog_end);
	free(m->read_end);
	free(m->prog_from);
	free(m->link);
	free(m->pg);
	free(m->heap);
	free(m->wait);
	free(m->held);
	free(m->spare);
	free(m->nflush);
	free(m->hist);
	free(m);
}

void ssd_model_submit(struct ssd_model *m, int tag, int op, __u64 lba,
		      __u64 nr)
{
	struct ssd_req r = { .tag = tag, .op = op, .lba = lba, .nr = nr };

	pump(m);
	r.arrive = r.t = m->now;
	start(m, r);
	pump(m);
}

void ssd_model_wake(struct ssd_model *m)
{
	pump(m);
}

int ssd_model_stats(const struct ssd_model *m, char *buf, int len)
{
	return snprintf(buf, len,
		"reads %llu\nwrites %llu\nflushes %llu\nflush_ms_sum %.3f\n"
		"flush_ms_max %.3f\nseq_write_mb %.3f\nrandom_write_mb %.3f\n"
		"program_units %.1f\nbuffer_full_waits %llu\n"
		"blocked_by_flush %llu\nblocked_ms_sum %.3f\n"
		"read_die_waits %llu\nread_die_wait_ms_sum %.3f\n"
		"read_suspends %llu\nfragment_reads %llu\nbuffer_mb %.3f\n"
		"gc_pool_mb %.3f\ngc_pool_used_mb %.3f\n",
		(unsigned long long)m->n_read, (unsigned long long)m->n_write,
		(unsigned long long)m->n_flush, m->flush_ns_sum / 1e6,
		m->flush_ns_max / 1e6, m->seq_bytes / 1048576.0,
		m->rnd_bytes / 1048576.0, m->units_done,
		(unsigned long long)m->n_buf_full,
		(unsigned long long)m->n_blocked, m->blocked_ns_sum / 1e6,
		(unsigned long long)m->n_read_wait, m->read_wait_ns_sum / 1e6,
		(unsigned long long)m->n_read_susp,
		(unsigned long long)m->n_frag_reads, m->buf_bytes / 1048576.0,
		m->pool / 1048576.0, m->pool_used / 1048576.0);
}
