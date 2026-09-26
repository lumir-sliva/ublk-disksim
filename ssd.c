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
 *    transfer over the flash channel; it waits for whatever the die is
 *    doing, a program included (no program suspend). Channels are not a
 *    shared resource: their total rate is above the host link on the
 *    drives this was fitted to;
 *  - one host link: each command occupies it for cmd_us plus its data at
 *    iface_mbps (reads after the flash read, writes on arrival), then
 *    iface_us of controller latency until completion;
 *  - a write buffer (buf_mb): writes complete once their data is in it.
 *    Full pages (page_kb) are programmed on whichever die is free first,
 *    as a log-structured drive does. A page of random writes costs `waf`
 *    program units instead of one: steady-state garbage collection folded
 *    into the page that caused it; writes continuing one of the last 8
 *    write streams count as sequential and cost one unit. A full buffer
 *    makes writes wait;
 *  - FLUSH: nothing is advertised with vwc = 0, so the kernel sends none.
 *    With plp = 1 the buffer is durable and a flush costs flush_us after
 *    it is issued; with plp = 0 the partly filled page is closed and the
 *    flush waits until every buffered page is programmed, plus flush_us
 *    (mapping-table commit). On SATA it is non-queued, as FLUSH CACHE:
 *    earlier commands finish first, everything arriving meanwhile, reads
 *    included, waits for it. On NVMe it is queued and holds nothing up;
 *  - floor_us: the host's own ublk overhead per request, measured on an
 *    empty target and subtracted from each completion (never below the
 *    request's arrival), so the parameters stay device latencies.
 *
 * Not modelled: garbage collection as a process (idle-time GC, fill level,
 * over-provisioning), SLC caching, program/erase suspend, reads served
 * from the write buffer, mapping-table cache misses, TRIM, thermal
 * throttling, multiple NVMe queues. Calibrate against the drive you want
 * to imitate before trusting absolute numbers.
 *
 * All state lives in one thread: the target requires -q 1 and one thread.
 */

#include "kublk.h"
#include <time.h>

enum { TD_DATA = 0, TD_MECH = 1, TD_DONE = 2 };
#define NTIMERS		64
#define NSTREAMS	8

struct ssd_req {
	int tag;
	__u8 op;
	__u8 rnd;		/* random write, costs waf units per page */
	__u64 lba, nr;		/* sectors */
	__u64 arrive;		/* reached the server */
	__u64 t;		/* reached the model (later if held by a flush) */
	__u64 link_end;		/* write: data is on the drive */
};

/* a closed buffer page waiting to be programmed or being programmed */
struct ssd_page {
	__u64 bytes, rnd;	/* rnd: bytes from random writes */
	__u64 ready;		/* closed at */
	double units;		/* program units not started yet */
	__u64 end;		/* end of its last unit */
	int freed;
};

struct ssd_iv {
	__u64 start, end;
};

struct ssd_flush {
	struct ssd_req r;
	__u64 seq;		/* done once pages up to seq are programmed */
};

struct ssd_model {
	struct ssd_params p;
	__u64 page_bytes, buf_cap;
	__u64 tr_ns, cmd_ns, iface_ns, flush_ns, floor_ns, unit_ns;

	struct ublk_thread *t;
	struct ublk_queue *q;
	__u64 now;

	__u64 *die_free;

	/* host link busy intervals, sorted, non-overlapping */
	struct ssd_iv *link;
	int nlink, cap_link;

	/* write buffer: open page + closed pages [seq_head, seq_next) */
	__u64 buf_bytes;
	__u64 open_bytes, open_rnd, open_last;
	struct ssd_page *pg;
	__u64 cap_pg;		/* power of two */
	__u64 seq_head;		/* oldest page not freed */
	__u64 seq_start;	/* oldest page with units not started */
	__u64 seq_next;
	__u64 max_end_started;	/* latest end over fully started pages */
	__u64 next_unit;	/* when the next unit can start, if any */
	__u64 *heap;		/* fully started pages, min-heap on end */
	__u64 nheap;

	/* write stream ends, for sequential detection */
	__u64 stream_end[NSTREAMS], stream_use[NSTREAMS], stream_clock;

	struct ssd_req *wait;	/* writes waiting for buffer space, FIFO */
	int nwait;

	/* SATA: the one pending non-queued flush and what it holds back */
	int sflush_tag;
	struct ssd_req sflush;
	int sflush_closed;
	__u64 sflush_seq;
	struct ssd_req *held;
	int nheld;
	__u64 block_until;
	__u64 last_done;

	/* NVMe with plp = 0: flushes waiting for pages, FIFO */
	struct ssd_flush *nflush;
	int nnflush;

	__u64 timer_busy;
	__u64 timer_when[NTIMERS];
	__u64 timer_at;
	struct __kernel_timespec timer_ts[NTIMERS];
	struct __kernel_timespec done_ts[UBLK_QUEUE_DEPTH];

	/* stats */
	int stats_fd;
	__u64 last_stats;
	__u64 n_read, n_write, n_flush, n_buf_full, n_blocked, n_read_wait;
	__u64 seq_bytes, rnd_bytes;
	double units_done;
	__u64 flush_ns_sum, flush_ns_max, blocked_ns_sum, read_wait_ns_sum;
};

static __u64 now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (__u64)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

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

static void arm_timer(struct ssd_model *m, __u64 when)
{
	struct io_uring_sqe *sqe;
	int slot;

	if (when >= m->timer_at || m->timer_busy == ~0ULL)
		return;
	slot = __builtin_ctzll(~m->timer_busy);
	m->timer_busy |= 1ULL << slot;
	m->timer_when[slot] = when;
	m->timer_at = when;
	m->timer_ts[slot].tv_sec = when / 1000000000ULL;
	m->timer_ts[slot].tv_nsec = when % 1000000000ULL;
	ublk_io_alloc_sqes(m->t, &sqe, 1);
	io_uring_prep_timeout(sqe, &m->timer_ts[slot], 0, IORING_TIMEOUT_ABS);
	sqe->user_data = build_user_data(slot, 0, TD_MECH, m->q->q_id, 1);
}

static void timer_fired(struct ssd_model *m, int slot)
{
	int i;

	m->timer_busy &= ~(1ULL << slot);
	m->timer_at = ~0ULL;
	for (i = 0; i < NTIMERS; i++)
		if ((m->timer_busy >> i & 1) && m->timer_when[i] < m->timer_at)
			m->timer_at = m->timer_when[i];
}

/* complete request r at model time `when`, less the host floor */
static void done(struct ssd_model *m, const struct ssd_req *r, __u64 when)
{
	struct io_uring_sqe *sqe;
	__u64 at = when > r->arrive + m->floor_ns ? when - m->floor_ns :
		r->arrive;

	m->last_done = max_u64(m->last_done, when);
	m->done_ts[r->tag].tv_sec = at / 1000000000ULL;
	m->done_ts[r->tag].tv_nsec = at % 1000000000ULL;
	ublk_io_alloc_sqes(m->t, &sqe, 1);
	io_uring_prep_timeout(sqe, &m->done_ts[r->tag], 0, IORING_TIMEOUT_ABS);
	sqe->user_data = build_user_data(r->tag, r->op, TD_DONE, m->q->q_id, 1);
}

static void flush_done(struct ssd_model *m, const struct ssd_req *r,
		       __u64 when)
{
	__u64 d = when - r->t;

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

/* the open page becomes a closed page, ready to program at `ready` */
static void close_open(struct ssd_model *m, __u64 ready)
{
	struct ssd_page *pg;

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
	pg = page(m, m->seq_next++);
	pg->bytes = m->open_bytes;
	pg->rnd = m->open_rnd;
	pg->ready = ready;
	pg->units = 1 + (m->p.waf - 1) * (double)m->open_rnd / m->open_bytes;
	pg->end = 0;
	pg->freed = 0;
	m->open_bytes = m->open_rnd = 0;
}

/* sequential if it continues one of the recent write streams */
static int classify(struct ssd_model *m, __u64 lba, __u64 nr)
{
	int i, lru = 0;

	m->stream_clock++;
	for (i = 0; i < NSTREAMS; i++) {
		if (m->stream_end[i] == lba && m->stream_use[i]) {
			m->stream_end[i] = lba + nr;
			m->stream_use[i] = m->stream_clock;
			return 0;
		}
		if (m->stream_use[i] < m->stream_use[lru])
			lru = i;
	}
	m->stream_end[lru] = lba + nr;
	m->stream_use[lru] = m->stream_clock;
	return 1;
}

/* write data enters the buffer at `a`; the write completes */
static void admit(struct ssd_model *m, const struct ssd_req *r, __u64 a)
{
	__u64 left = r->nr << 9;

	m->buf_bytes += left;
	while (left) {
		__u64 take = min_u64(left, m->page_bytes - m->open_bytes);

		m->open_bytes += take;
		if (r->rnd)
			m->open_rnd += take;
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
	struct ssd_req *held;
	int i, n = m->nheld;

	flush_done(m, &m->sflush, when);
	m->sflush_tag = -1;
	m->block_until = max_u64(m->block_until, when);

	held = malloc(n * sizeof(*held) + 1);
	memcpy(held, m->held, n * sizeof(*held));
	m->nheld = 0;
	for (i = 0; i < n; i++)
		start(m, held[i]);
	free(held);
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
	if (!m->p.plp) {
		if (!m->sflush_closed) {
			/*
			 * The drive programs a partial page only because of the
			 * flush, so not before it; and it's a new page to
			 * program: let run_units see it.
			 */
			close_open(m, max_u64(m->open_last, m->sflush.t));
			m->sflush_closed = 1;
			m->sflush_seq = m->seq_next;
			return 1;
		}
		if (m->seq_start < m->sflush_seq)
			return 0;
		drain = max_u64(drain, m->max_end_started);
	}
	sata_flush_finish(m, drain + m->flush_ns);
	return 1;
}

static void write_stats(struct ssd_model *m)
{
	char buf[1024];
	int n;

	if (m->stats_fd < 0 || m->now - m->last_stats < 1000000000ULL)
		return;
	m->last_stats = m->now;
	n = snprintf(buf, sizeof(buf),
		"reads %llu\nwrites %llu\nflushes %llu\nflush_ms_sum %.3f\n"
		"flush_ms_max %.3f\nseq_write_mb %.3f\nrandom_write_mb %.3f\n"
		"program_units %.1f\nbuffer_full_waits %llu\n"
		"blocked_by_flush %llu\nblocked_ms_sum %.3f\n"
		"read_die_waits %llu\nread_die_wait_ms_sum %.3f\n"
		"buffer_mb %.3f\n",
		(unsigned long long)m->n_read, (unsigned long long)m->n_write,
		(unsigned long long)m->n_flush, m->flush_ns_sum / 1e6,
		m->flush_ns_max / 1e6, m->seq_bytes / 1048576.0,
		m->rnd_bytes / 1048576.0, m->units_done,
		(unsigned long long)m->n_buf_full,
		(unsigned long long)m->n_blocked, m->blocked_ns_sum / 1e6,
		(unsigned long long)m->n_read_wait, m->read_wait_ns_sum / 1e6,
		m->buf_bytes / 1048576.0);
	if (pwrite(m->stats_fd, buf, n, 0) == n &&
	    ftruncate(m->stats_fd, n) < 0)
		m->stats_fd = -1;
}

/* catch the model up to now and arm a timer for its next event */
static void pump(struct ssd_model *m)
{
	__u64 next;

	m->now = now_ns();
	link_prune(m);
	while (run_units(m) | free_pages(m) | sata_flush_check(m))
		;
	next = m->next_unit;
	if (m->nwait && m->nheap)
		next = min_u64(next, page(m, m->heap[0])->end);
	if (next != ~0ULL)
		arm_timer(m, next);
	write_stats(m);
}

static void do_read(struct ssd_model *m, const struct ssd_req *r)
{
	__u64 off = r->lba << 9, end = off + (r->nr << 9), fdone = r->t, p;

	m->n_read++;
	for (p = off / m->page_bytes; p * m->page_bytes < end; p++) {
		__u64 s0 = max_u64(p * m->page_bytes, off);
		__u64 e0 = min_u64((p + 1) * m->page_bytes, end);
		__u64 *df = &m->die_free[p % m->p.dies];
		__u64 s = max_u64(*df, r->t);

		if (s > r->t) {
			m->n_read_wait++;
			m->read_wait_ns_sum += s - r->t;
		}
		*df = s + m->tr_ns + xfer_ns(m->p.ch_mbps, e0 - s0);
		fdone = max_u64(fdone, *df);
	}
	done(m, r, link_reserve(m, fdone, m->cmd_ns +
				xfer_ns(m->p.iface_mbps, r->nr << 9)) +
	     m->iface_ns);
}

static void do_write(struct ssd_model *m, struct ssd_req r)
{
	__u64 bytes = r.nr << 9;

	m->n_write++;
	r.link_end = link_reserve(m, r.t, m->cmd_ns +
				  xfer_ns(m->p.iface_mbps, bytes));
	r.rnd = classify(m, r.lba, r.nr);
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
	case UBLK_IO_OP_READ:
		do_read(m, &r);
		break;
	case UBLK_IO_OP_WRITE:
		do_write(m, r);
		break;
	case UBLK_IO_OP_FLUSH:
		do_flush(m, &r);
		break;
	default:
		assert(0);
	}
}

static int ssd_queue_io(struct ublk_thread *t, struct ublk_queue *q, int tag)
{
	const struct ublksrv_io_desc *iod = ublk_get_iod(q, tag);
	struct ssd_model *m = q->dev->private_data;
	unsigned op = ublksrv_get_op(iod);
	struct ssd_req r = {
		.tag = tag,
		.op = op,
		.lba = iod->start_sector,
		.nr = iod->nr_sectors,
	};
	struct io_uring_sqe *sqe;

	m->t = t;
	m->q = q;

	switch (op) {
	case UBLK_IO_OP_READ:
	case UBLK_IO_OP_WRITE:
		ublk_io_alloc_sqes(t, &sqe, 1);
		io_uring_prep_rw(op == UBLK_IO_OP_READ ? IORING_OP_READ :
				 IORING_OP_WRITE, sqe,
				 ublk_get_registered_fd(q, 1),
				 (void *)iod->addr, iod->nr_sectors << 9,
				 iod->start_sector << 9);
		io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		sqe->user_data = build_user_data(tag, op, TD_DATA, q->q_id, 1);
		ublk_queued_tgt_io(t, q, tag, 2);
		break;
	case UBLK_IO_OP_FLUSH:
		ublk_queued_tgt_io(t, q, tag, 1);
		break;
	default:
		ublk_queued_tgt_io(t, q, tag, -EOPNOTSUPP);
		return 0;
	}

	pump(m);
	r.arrive = r.t = m->now;
	start(m, r);
	pump(m);
	return 0;
}

static void ssd_io_done(struct ublk_thread *t, struct ublk_queue *q,
			const struct io_uring_cqe *cqe)
{
	struct ssd_model *m = q->dev->private_data;
	unsigned td = user_data_to_tgt_data(cqe->user_data);
	unsigned tag = user_data_to_tag(cqe->user_data);
	struct ublk_io *io;

	if (td == TD_MECH) {
		timer_fired(m, tag);
		pump(m);
		return;
	}

	io = ublk_get_io(q, tag);
	if (td == TD_DATA) {
		if (cqe->res < 0 || !io->result)
			io->result = cqe->res;
		if (cqe->res < 0)
			ublk_err("%s: data io failed tag %u res %d\n",
				 __func__, tag, cqe->res);
	} else if (cqe->res != -ETIME) {
		ublk_err("%s: timer tag %u res %d\n", __func__, tag, cqe->res);
	}

	if (ublk_completed_tgt_io(t, q, tag))
		ublk_complete_io(t, q, tag, io->result);
}

static int ssd_init_tgt(const struct dev_ctx *ctx, struct ublk_dev *dev)
{
	const struct ssd_params *p = &ctx->ssd;
	unsigned depth = dev->dev_info.queue_depth;
	struct ssd_model *m;
	__u64 bytes;
	int ret;

	if (dev->dev_info.nr_hw_queues != 1 || ctx->nthreads > 1) {
		ublk_err("ssd: one model thread, needs -q 1 and one thread\n");
		return -EINVAL;
	}
	if (dev->dev_info.flags & (UBLK_F_SUPPORT_ZERO_COPY |
				   UBLK_F_AUTO_BUF_REG)) {
		ublk_err("ssd: zero copy not supported\n");
		return -EINVAL;
	}
	if (p->dies == 0 || p->page_kb < 4 || p->tr_us <= 0 ||
	    p->tprog_us <= 0 || p->ch_mbps <= 0 || p->iface_mbps <= 0 ||
	    p->waf < 1 || p->cmd_us < 0 || p->iface_us < 0 ||
	    p->flush_us < 0 || p->floor_us < 0 ||
	    ((__u64)p->buf_mb << 20) < dev->dev_info.max_io_buf_bytes +
	    ((__u64)p->page_kb << 10)) {
		ublk_err("ssd: bad model parameters (buf_mb must hold the "
			 "largest request plus a page)\n");
		return -EINVAL;
	}

	ret = backing_file_tgt_init(dev);
	if (ret)
		return ret;
	if (dev->tgt.nr_backing_files != 1)
		return -EINVAL;

	bytes = dev->tgt.backing_file_size[0];
	dev->tgt.dev_size = bytes;
	dev->tgt.params = (struct ublk_params) {
		.types = UBLK_PARAM_TYPE_BASIC | UBLK_PARAM_TYPE_DMA_ALIGN,
		.basic = {
			.attrs = p->vwc ? UBLK_ATTR_VOLATILE_CACHE : 0,
			.logical_bs_shift	= 9,
			.physical_bs_shift	= 12,
			.io_opt_shift		= 12,
			.io_min_shift		= 12,
			.max_sectors = dev->dev_info.max_io_buf_bytes >> 9,
			.dev_sectors		= bytes >> 9,
		},
		.dma = {
			.alignment = 511,
		},
	};
	/* per request: data io + completion timer, plus model timers */
	dev->tgt.sq_depth = dev->tgt.cq_depth = 4 * depth + NTIMERS + 8;

	m = calloc(1, sizeof(*m));
	m->p = *p;
	m->page_bytes = (__u64)p->page_kb << 10;
	m->buf_cap = (__u64)p->buf_mb << 20;
	m->tr_ns = (__u64)(p->tr_us * 1e3);
	m->cmd_ns = (__u64)(p->cmd_us * 1e3);
	m->iface_ns = (__u64)(p->iface_us * 1e3);
	m->flush_ns = (__u64)(p->flush_us * 1e3);
	m->floor_ns = (__u64)(p->floor_us * 1e3);
	m->unit_ns = xfer_ns(p->ch_mbps, m->page_bytes) +
		(__u64)(p->tprog_us * 1e3);
	m->die_free = calloc(p->dies, sizeof(*m->die_free));
	m->cap_link = 2 * depth;
	m->link = calloc(m->cap_link, sizeof(*m->link));
	m->cap_pg = 1024;
	m->pg = calloc(m->cap_pg, sizeof(*m->pg));
	m->heap = calloc(m->cap_pg, sizeof(*m->heap));
	m->wait = calloc(depth, sizeof(*m->wait));
	m->held = calloc(depth, sizeof(*m->held));
	m->nflush = calloc(depth, sizeof(*m->nflush));
	m->sflush_tag = -1;
	m->timer_at = ~0ULL;
	m->stats_fd = -1;
	if (p->stats[0]) {
		m->stats_fd = open(p->stats, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (m->stats_fd < 0)
			ublk_err("ssd: can't open stats file %s: %m\n", p->stats);
	}
	dev->private_data = m;

	ublk_log("ssd: %s link %.0f MB/s cmd %.1f us latency %.1f us, %u dies, "
		 "page %u KiB, read %.1f us, program %.1f us x waf %.2f, "
		 "channel %.0f MB/s, buffer %u MiB, plp %u vwc %u flush %.1f us, "
		 "floor %.1f us\n",
		 p->nvme ? "nvme" : "sata", p->iface_mbps, p->cmd_us,
		 p->iface_us, p->dies, p->page_kb, p->tr_us, p->tprog_us,
		 p->waf, p->ch_mbps, p->buf_mb, p->plp, p->vwc, p->flush_us,
		 p->floor_us);
	return 0;
}

static void ssd_deinit_tgt(struct ublk_dev *dev)
{
	struct ssd_model *m = dev->private_data;

	backing_file_tgt_deinit(dev);
	if (!m)
		return;
	if (m->stats_fd >= 0) {
		m->last_stats = 0;
		m->now = ~0ULL;
		write_stats(m);
		close(m->stats_fd);
	}
	free(m->die_free);
	free(m->link);
	free(m->pg);
	free(m->heap);
	free(m->wait);
	free(m->held);
	free(m->nflush);
	free(m);
}

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
};

static void ssd_cmd_line(struct dev_ctx *ctx, int argc, char *argv[])
{
	struct ssd_params *p = &ctx->ssd;
	int i, j;

	*p = profiles[0].p;
	/* profile first, so explicit parameters override it in any order */
	for (i = 1; i + 1 < argc; i += 2)
		if (!strcmp(argv[i], "--profile")) {
			for (j = 0; j < (int)ARRAY_SIZE(profiles); j++)
				if (!strcmp(profiles[j].name, argv[i + 1]))
					break;
			if (j == (int)ARRAY_SIZE(profiles)) {
				ublk_err("ssd: unknown profile %s\n", argv[i + 1]);
				exit(EXIT_FAILURE);
			}
			*p = profiles[j].p;
		}

	for (i = 1; i + 1 < argc; i += 2) {
		const char *k = argv[i], *v = argv[i + 1];

		if (!strcmp(k, "--profile"))
			continue;
		else if (!strcmp(k, "--iface")) {
			if (strcmp(v, "sata") && strcmp(v, "nvme")) {
				ublk_err("ssd: --iface sata or nvme\n");
				exit(EXIT_FAILURE);
			}
			p->nvme = !strcmp(v, "nvme");
		} else if (!strcmp(k, "--iface_mbps"))
			p->iface_mbps = strtod(v, NULL);
		else if (!strcmp(k, "--cmd_us"))
			p->cmd_us = strtod(v, NULL);
		else if (!strcmp(k, "--iface_us"))
			p->iface_us = strtod(v, NULL);
		else if (!strcmp(k, "--dies"))
			p->dies = strtoul(v, NULL, 10);
		else if (!strcmp(k, "--page_kb"))
			p->page_kb = strtoul(v, NULL, 10);
		else if (!strcmp(k, "--tr_us"))
			p->tr_us = strtod(v, NULL);
		else if (!strcmp(k, "--ch_mbps"))
			p->ch_mbps = strtod(v, NULL);
		else if (!strcmp(k, "--tprog_us"))
			p->tprog_us = strtod(v, NULL);
		else if (!strcmp(k, "--waf"))
			p->waf = strtod(v, NULL);
		else if (!strcmp(k, "--buf_mb"))
			p->buf_mb = strtoul(v, NULL, 10);
		else if (!strcmp(k, "--plp"))
			p->plp = strtoul(v, NULL, 10);
		else if (!strcmp(k, "--vwc"))
			p->vwc = strtoul(v, NULL, 10);
		else if (!strcmp(k, "--flush_us"))
			p->flush_us = strtod(v, NULL);
		else if (!strcmp(k, "--floor_us"))
			p->floor_us = strtod(v, NULL);
		else if (!strcmp(k, "--stats"))
			snprintf(p->stats, sizeof(p->stats), "%s", v);
		/* other targets' options pass through here too: ignore */
	}
}

static void ssd_usage(const struct ublk_tgt_ops *ops)
{
	printf("\tssd: [--profile sata-plp|nvme-plp|sata-consumer] "
	       "[--iface sata|nvme]\n"
	       "\t     [--iface_mbps X] [--cmd_us X] [--iface_us X] [--dies N] "
	       "[--page_kb N]\n"
	       "\t     [--tr_us X] [--ch_mbps X] [--tprog_us X] [--waf X] "
	       "[--buf_mb N]\n"
	       "\t     [--plp 0|1] [--vwc 0|1] [--flush_us X] [--floor_us X] "
	       "[--stats FILE]\n"
	       "\t     BACKING_DEV (use -q 1; -d 32 for sata, -d 128 for nvme)\n");
}

const struct ublk_tgt_ops ssd_tgt_ops = {
	.name = "ssd",
	.init_tgt = ssd_init_tgt,
	.deinit_tgt = ssd_deinit_tgt,
	.queue_io = ssd_queue_io,
	.tgt_io_done = ssd_io_done,
	.parse_cmd_line = ssd_cmd_line,
	.usage = ssd_usage,
};
