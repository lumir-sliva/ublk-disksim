// SPDX-License-Identifier: GPL-2.0
/*
 * hdd: timing model of a single-actuator hard disk, on top of a backing
 * device (normally RAM).
 *
 * Data goes to the backing device immediately; the model only decides when
 * each request completes. What it models:
 *
 *  - one actuator: reads and write-through writes are served one at a time,
 *    shortest-positioning-time-first among the oldest `ncq` waiting
 *    requests (NCQ with rotational position ordering);
 *  - positioning = seek(distance, none within a track) + wait for the
 *    sector to rotate under the head (sector angle = lba / sectors per
 *    track, platter phase = time / revolution), then media transfer; a
 *    request that starts where the head is costs no positioning;
 *  - a volatile write cache (cache_mb > 0): writes complete after the host
 *    transfer and are destaged by the actuator when it has nothing else to
 *    do, shortest-positioning-time-first over the whole cache; when the
 *    cache is full, writes wait for space;
 *  - FLUSH as SATA FLUSH CACHE: a non-queued command. Requests queued before
 *    it are served, the whole dirty cache is destaged, and everything that
 *    arrives meanwhile (reads included) waits until the flush completes;
 *  - write-through (cache_mb = 0): no volatile cache is advertised, so the
 *    kernel never sends flushes and every write pays mechanical time.
 *
 * Not modelled: zoned transfer rates (one rate, one track size), read
 * cache / read-ahead beyond "sequential costs no positioning", firmware
 * limits on dirty data, destage idle timers, thermal recalibration.
 * Calibrate against the drive you want to imitate before trusting
 * absolute numbers.
 *
 * All state lives in one thread: the target requires -q 1 and one thread.
 */

#include "kublk.h"
#include <math.h>
#include <time.h>

enum { TD_DATA = 0, TD_MECH = 1, TD_DONE = 2 };
#define MECH_TAG 0xffff

struct hdd_req {
	int tag;
	__u8 op;
	__u64 lba, nr;		/* sectors */
	__u64 arrive;
};

struct hdd_ext {
	__u64 lba, nr;		/* sectors */
	__u64 added;		/* when it entered the cache */
};

struct hdd_model {
	struct hdd_params p;
	double seek_full_ms;
	double period_ns;	/* one revolution */
	double spt;		/* sectors per track, from media rate and rpm */
	double phase0;		/* platter phase at t = 0, from the seed */
	__u64 dev_sectors;
	__u64 cache_bytes;

	struct ublk_thread *t;
	struct ublk_queue *q;

	__u64 head;
	__u64 busy_until;
	int timer_armed;
	struct __kernel_timespec mech_ts;
	struct __kernel_timespec done_ts[UBLK_QUEUE_DEPTH];

	/* waiting for the actuator: reads, write-through writes */
	struct hdd_req *pend;
	int npend;
	/* writes waiting for cache space, FIFO */
	struct hdd_req *wait;
	int nwait;
	/* arrived while a flush was in progress, FIFO */
	struct hdd_req *blocked;
	int nblocked;

	int flush_tag;
	__u64 flush_start;

	struct hdd_ext *dirty;
	int ndirty, cap_dirty;
	__u64 dirty_bytes;

	/* stats */
	int stats_fd;
	__u64 last_stats;
	__u64 n_read, n_write, n_flush, n_destage, n_cache_full;
	__u64 flush_ns_sum, flush_ns_max, flush_bytes_sum;
	__u64 n_blocked, blocked_ns_sum;
};

static __u64 now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (__u64)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* platter phase in [0, 1) at time t */
static double phase_at(struct hdd_model *m, double t)
{
	double x = t / m->period_ns + m->phase0;

	return x - floor(x);
}

/*
 * Time from `t` until the head can start transferring `to`, coming from
 * the end of the previous transfer at `from`. If `contig` and the request
 * continues where the head is, it costs nothing: reads (read-ahead),
 * destage (the drive's own queue) and writes that were already queued
 * when the previous transfer ended. Otherwise: seek (none within the same
 * track), then wait for the sector to come round; for a write-through
 * write arriving after the previous one completed, that is most of a
 * revolution, as on a real drive. Sector angle is lba / spt, so a
 * transfer ending at lba e ends at phase e / spt, and choosing the minimum
 * of this over queued requests is rotational position ordering.
 */
static double pos_ns(struct hdd_model *m, double t, __u64 from, __u64 to,
		     int contig)
{
	double seek = 0, ang, rot;

	if (from == to && contig)
		return 0;
	if ((__u64)(from / m->spt) != (__u64)(to / m->spt)) {
		double d = fabs((double)to - (double)from) /
			(double)m->dev_sectors * m->p.stroke;

		if (d > 1)
			d = 1;
		seek = (m->p.seek_min_ms + (m->seek_full_ms - m->p.seek_min_ms) *
			sqrt(d)) * 1e6;
	}
	ang = (double)to / m->spt;
	ang -= floor(ang);
	rot = ang - phase_at(m, t + seek);
	if (rot < 0)
		rot += 1;
	return seek + rot * m->period_ns;
}

static __u64 xfer_ns(double mbps, __u64 nr)
{
	return (__u64)((double)(nr << 9) / (mbps * 1e6) * 1e9);
}

static __u64 host_ns(struct hdd_model *m, __u64 nr)
{
	return (__u64)(m->p.iface_us * 1e3) + xfer_ns(m->p.iface_mbps, nr);
}

static void submit_done(struct hdd_model *m, int tag, __u8 op, __u64 when)
{
	struct io_uring_sqe *sqe;

	m->done_ts[tag].tv_sec = when / 1000000000ULL;
	m->done_ts[tag].tv_nsec = when % 1000000000ULL;
	ublk_io_alloc_sqes(m->t, &sqe, 1);
	io_uring_prep_timeout(sqe, &m->done_ts[tag], 0, IORING_TIMEOUT_ABS);
	sqe->user_data = build_user_data(tag, op, TD_DONE, m->q->q_id, 1);
}

static void arm_mech(struct hdd_model *m)
{
	struct io_uring_sqe *sqe;

	if (m->timer_armed)
		return;
	m->mech_ts.tv_sec = m->busy_until / 1000000000ULL;
	m->mech_ts.tv_nsec = m->busy_until % 1000000000ULL;
	ublk_io_alloc_sqes(m->t, &sqe, 1);
	io_uring_prep_timeout(sqe, &m->mech_ts, 0, IORING_TIMEOUT_ABS);
	sqe->user_data = build_user_data(MECH_TAG, 0, TD_MECH, m->q->q_id, 1);
	m->timer_armed = 1;
}

/* add [lba, lba+nr) to the dirty set, merging overlapping/adjacent extents */
static void dirty_add(struct hdd_model *m, __u64 lba, __u64 nr, __u64 t)
{
	__u64 s = lba, e = lba + nr;
	int i = 0;

	while (i < m->ndirty) {
		struct hdd_ext *x = &m->dirty[i];

		if (x->lba <= e && s <= x->lba + x->nr) {
			if (x->lba < s)
				s = x->lba;
			if (x->lba + x->nr > e)
				e = x->lba + x->nr;
			m->dirty_bytes -= x->nr << 9;
			m->dirty[i] = m->dirty[--m->ndirty];
			continue;
		}
		i++;
	}
	if (m->ndirty == m->cap_dirty) {
		m->cap_dirty *= 2;
		m->dirty = realloc(m->dirty, m->cap_dirty * sizeof(*m->dirty));
		assert(m->dirty);
	}
	m->dirty[m->ndirty].lba = s;
	m->dirty[m->ndirty].nr = e - s;
	m->dirty[m->ndirty].added = t;
	m->ndirty++;
	m->dirty_bytes += (e - s) << 9;
}

static void write_stats(struct hdd_model *m, __u64 now)
{
	char buf[1024];
	int n;

	if (m->stats_fd < 0 || now - m->last_stats < 1000000000ULL)
		return;
	m->last_stats = now;
	n = snprintf(buf, sizeof(buf),
		"reads %llu\nwrites %llu\nflushes %llu\nflush_ms_sum %.3f\n"
		"flush_ms_max %.3f\nflush_dirty_mb_sum %.3f\ndestaged %llu\n"
		"cache_full_waits %llu\nblocked_by_flush %llu\n"
		"blocked_ms_sum %.3f\ndirty_mb %.3f\n",
		(unsigned long long)m->n_read, (unsigned long long)m->n_write,
		(unsigned long long)m->n_flush, m->flush_ns_sum / 1e6,
		m->flush_ns_max / 1e6, m->flush_bytes_sum / 1048576.0,
		(unsigned long long)m->n_destage,
		(unsigned long long)m->n_cache_full,
		(unsigned long long)m->n_blocked, m->blocked_ns_sum / 1e6,
		m->dirty_bytes / 1048576.0);
	if (pwrite(m->stats_fd, buf, n, 0) == n &&
	    ftruncate(m->stats_fd, n) < 0)
		m->stats_fd = -1;
}

static void arrive(struct hdd_model *m, struct hdd_req r, __u64 now);

static __u64 max_u64(__u64 a, __u64 b)
{
	return a > b ? a : b;
}

/*
 * Run the actuator up to `now`. The actuator keeps its own timeline:
 * each operation starts when the actuator became free or when its request
 * arrived, whichever is later, not when this function happens to run, so
 * the model doesn't depend on how promptly timers fire. Completions that
 * fall in the (recent) past fire at once.
 */
static void pump(struct hdd_model *m)
{
	__u64 now = now_ns();

	for (;;) {
		__u64 free_at = m->busy_until;

		if (free_at > now) {
			arm_mech(m);
			break;
		}

		/* cache space freed by destaging admits waiting writes */
		while (m->nwait && m->dirty_bytes + (m->wait[0].nr << 9) <=
				m->cache_bytes) {
			struct hdd_req w = m->wait[0];
			__u64 t = max_u64(free_at, w.arrive);

			memmove(m->wait, m->wait + 1, --m->nwait * sizeof(*m->wait));
			dirty_add(m, w.lba, w.nr, t);
			submit_done(m, w.tag, w.op, t + host_ns(m, w.nr));
		}

		if (m->npend) {
			int n = m->npend < (int)m->p.ncq ? m->npend : (int)m->p.ncq;
			int i, best = -1;
			double ready, bready = 0;
			struct hdd_req r;

			/* earliest start of transfer first (NCQ with RPO) */
			for (i = 0; i < n; i++) {
				const struct hdd_req *c = &m->pend[i];
				__u64 t = max_u64(free_at, c->arrive);
				int contig = c->op == UBLK_IO_OP_READ ||
					c->arrive <= free_at;

				ready = t + pos_ns(m, t, m->head, c->lba, contig);
				if (best < 0 || ready < bready) {
					bready = ready;
					best = i;
				}
			}
			r = m->pend[best];
			memmove(m->pend + best, m->pend + best + 1,
				(--m->npend - best) * sizeof(*m->pend));
			m->busy_until = (__u64)bready + xfer_ns(m->p.mbps, r.nr);
			m->head = r.lba + r.nr;
			submit_done(m, r.tag, r.op, m->busy_until);
			continue;
		}

		if (m->flush_tag >= 0 && !m->ndirty && !m->nwait) {
			__u64 t = max_u64(free_at, m->flush_start);
			__u64 d = t - m->flush_start;
			struct hdd_req *held;
			int i, n = m->nblocked;

			submit_done(m, m->flush_tag, UBLK_IO_OP_FLUSH, t);
			m->n_flush++;
			m->flush_ns_sum += d;
			if (d > m->flush_ns_max)
				m->flush_ns_max = d;
			m->flush_tag = -1;

			/* replay what the flush held back, in arrival order */
			held = malloc(n * sizeof(*held) + 1);
			memcpy(held, m->blocked, n * sizeof(*held));
			m->nblocked = 0;
			for (i = 0; i < n; i++) {
				m->n_blocked++;
				m->blocked_ns_sum += t - held[i].arrive;
				arrive(m, held[i], t);
			}
			free(held);
			continue;
		}

		if (m->ndirty) {
			int i, best = -1;
			double ready, bready = 0;
			struct hdd_ext x;

			for (i = 0; i < m->ndirty; i++) {
				__u64 t = max_u64(free_at, m->dirty[i].added);

				ready = t + pos_ns(m, t, m->head, m->dirty[i].lba, 1);
				if (best < 0 || ready < bready) {
					bready = ready;
					best = i;
				}
			}
			x = m->dirty[best];
			m->dirty[best] = m->dirty[--m->ndirty];
			m->dirty_bytes -= x.nr << 9;
			m->busy_until = (__u64)bready + xfer_ns(m->p.mbps, x.nr);
			m->head = x.lba + x.nr;
			m->n_destage++;
			continue;
		}
		break;
	}
	write_stats(m, now);
}

/* a request reaches the drive at `now` (or is released by a flush then) */
static void arrive(struct hdd_model *m, struct hdd_req r, __u64 now)
{
	r.arrive = now;
	if (m->flush_tag >= 0) {
		m->blocked[m->nblocked++] = r;
		return;
	}

	switch (r.op) {
	case UBLK_IO_OP_READ:
		m->n_read++;
		m->pend[m->npend++] = r;
		break;
	case UBLK_IO_OP_WRITE:
		m->n_write++;
		if (!m->cache_bytes) {
			m->pend[m->npend++] = r;
		} else if (!m->nwait && m->dirty_bytes + (r.nr << 9) <=
				m->cache_bytes) {
			dirty_add(m, r.lba, r.nr, now);
			submit_done(m, r.tag, r.op, now + host_ns(m, r.nr));
		} else {
			m->n_cache_full++;
			m->wait[m->nwait++] = r;
		}
		break;
	case UBLK_IO_OP_FLUSH:
		if (!m->cache_bytes) {
			submit_done(m, r.tag, r.op, now);
			break;
		}
		m->flush_tag = r.tag;
		m->flush_start = now;
		m->flush_bytes_sum += m->dirty_bytes;
		break;
	default:
		assert(0);
	}
}

static int hdd_queue_io(struct ublk_thread *t, struct ublk_queue *q, int tag)
{
	const struct ublksrv_io_desc *iod = ublk_get_iod(q, tag);
	struct hdd_model *m = q->dev->private_data;
	unsigned op = ublksrv_get_op(iod);
	struct hdd_req r = {
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

	arrive(m, r, now_ns());
	pump(m);
	return 0;
}

static void hdd_io_done(struct ublk_thread *t, struct ublk_queue *q,
			const struct io_uring_cqe *cqe)
{
	struct hdd_model *m = q->dev->private_data;
	unsigned td = user_data_to_tgt_data(cqe->user_data);
	unsigned tag = user_data_to_tag(cqe->user_data);
	struct ublk_io *io;

	if (td == TD_MECH) {
		m->timer_armed = 0;
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

static int hdd_init_tgt(const struct dev_ctx *ctx, struct ublk_dev *dev)
{
	const struct hdd_params *p = &ctx->hdd;
	unsigned depth = dev->dev_info.queue_depth;
	struct hdd_model *m;
	__u64 bytes;
	int ret;

	if (dev->dev_info.nr_hw_queues != 1 || ctx->nthreads > 1) {
		ublk_err("hdd: one actuator, needs -q 1 and one thread\n");
		return -EINVAL;
	}
	if (dev->dev_info.flags & (UBLK_F_SUPPORT_ZERO_COPY |
				   UBLK_F_AUTO_BUF_REG)) {
		ublk_err("hdd: zero copy not supported\n");
		return -EINVAL;
	}
	if (p->seek_avg_ms < p->seek_min_ms || p->rpm == 0 || p->mbps <= 0 ||
	    p->iface_mbps <= 0 || p->stroke <= 0 || p->ncq == 0) {
		ublk_err("hdd: bad model parameters\n");
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
			.attrs = UBLK_ATTR_ROTATIONAL |
				(p->cache_mb ? UBLK_ATTR_VOLATILE_CACHE : 0),
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
	/* per request: data io + completion timer, plus the actuator timer */
	dev->tgt.sq_depth = dev->tgt.cq_depth = 4 * depth + 8;

	m = calloc(1, sizeof(*m));
	m->p = *p;
	/* sqrt seek curve: mean over uniform random pairs is 8/15 of full */
	m->seek_full_ms = p->seek_min_ms +
		(p->seek_avg_ms - p->seek_min_ms) * 15.0 / 8.0;
	m->dev_sectors = bytes >> 9;
	m->cache_bytes = (__u64)p->cache_mb << 20;
	m->pend = calloc(depth, sizeof(*m->pend));
	m->wait = calloc(depth, sizeof(*m->wait));
	m->blocked = calloc(depth, sizeof(*m->blocked));
	m->cap_dirty = 1024;
	m->dirty = calloc(m->cap_dirty, sizeof(*m->dirty));
	m->flush_tag = -1;
	m->period_ns = 60e9 / p->rpm;
	m->spt = p->mbps * 1e6 * (60.0 / p->rpm) / 512;
	m->phase0 = (double)(p->seed % 1000003) / 1000003.0;
	m->stats_fd = -1;
	if (p->stats[0]) {
		m->stats_fd = open(p->stats, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (m->stats_fd < 0)
			ublk_err("hdd: can't open stats file %s: %m\n", p->stats);
	}
	dev->private_data = m;

	ublk_log("hdd: rpm %u seek %.2f/%.2f/%.2f ms (min/avg/full) media %.0f MB/s "
		 "(%.0f sectors/track) cache %u MiB ncq %u stroke %.2f\n",
		 p->rpm, p->seek_min_ms, p->seek_avg_ms, m->seek_full_ms,
		 p->mbps, m->spt, p->cache_mb, p->ncq, p->stroke);
	return 0;
}

static void hdd_deinit_tgt(struct ublk_dev *dev)
{
	struct hdd_model *m = dev->private_data;

	backing_file_tgt_deinit(dev);
	if (!m)
		return;
	if (m->stats_fd >= 0) {
		m->last_stats = 0;
		write_stats(m, ~0ULL);
		close(m->stats_fd);
	}
	free(m->pend);
	free(m->wait);
	free(m->blocked);
	free(m->dirty);
	free(m);
}

/*
 * Profiles: named parameter sets. hgst-7k8 approximates the fleet's
 * HGST Ultrastar 7K8 (HUS728T8TALE6L4, 8 TB SATA, 7200 rpm); see README
 * for the sources and the calibration status of each number.
 */
static const struct {
	const char *name;
	struct hdd_params p;
} profiles[] = {
	{ "hgst-7k8", {
		.rpm = 7200, .seek_min_ms = 0.6, .seek_avg_ms = 8.0,
		.mbps = 205, .iface_mbps = 600, .iface_us = 30,
		.cache_mb = 64, .ncq = 32, .stroke = 1.0,
	} },
};

static void hdd_cmd_line(struct dev_ctx *ctx, int argc, char *argv[])
{
	struct hdd_params *p = &ctx->hdd;
	int i, j;

	*p = profiles[0].p;
	/* profile first, so explicit parameters override it in any order */
	for (i = 1; i + 1 < argc; i += 2)
		if (!strcmp(argv[i], "--profile")) {
			for (j = 0; j < (int)ARRAY_SIZE(profiles); j++)
				if (!strcmp(profiles[j].name, argv[i + 1]))
					break;
			if (j == (int)ARRAY_SIZE(profiles)) {
				ublk_err("hdd: unknown profile %s\n", argv[i + 1]);
				exit(EXIT_FAILURE);
			}
			*p = profiles[j].p;
		}

	for (i = 1; i + 1 < argc; i += 2) {
		const char *k = argv[i], *v = argv[i + 1];

		if (!strcmp(k, "--profile"))
			continue;
		else if (!strcmp(k, "--rpm"))
			p->rpm = strtoul(v, NULL, 10);
		else if (!strcmp(k, "--seek_min_ms"))
			p->seek_min_ms = strtod(v, NULL);
		else if (!strcmp(k, "--seek_avg_ms"))
			p->seek_avg_ms = strtod(v, NULL);
		else if (!strcmp(k, "--mbps"))
			p->mbps = strtod(v, NULL);
		else if (!strcmp(k, "--iface_mbps"))
			p->iface_mbps = strtod(v, NULL);
		else if (!strcmp(k, "--iface_us"))
			p->iface_us = strtod(v, NULL);
		else if (!strcmp(k, "--cache_mb"))
			p->cache_mb = strtoul(v, NULL, 10);
		else if (!strcmp(k, "--ncq"))
			p->ncq = strtoul(v, NULL, 10);
		else if (!strcmp(k, "--stroke"))
			p->stroke = strtod(v, NULL);
		else if (!strcmp(k, "--seed"))
			p->seed = strtoull(v, NULL, 10);
		else if (!strcmp(k, "--stats"))
			snprintf(p->stats, sizeof(p->stats), "%s", v);
		/* other targets' options pass through here too: ignore */
	}
}

static void hdd_usage(const struct ublk_tgt_ops *ops)
{
	printf("\thdd: [--profile hgst-7k8] [--rpm N] [--seek_min_ms X] "
	       "[--seek_avg_ms X]\n"
	       "\t     [--mbps X] [--iface_mbps X] [--iface_us X] "
	       "[--cache_mb N (0 = write-through)]\n"
	       "\t     [--ncq N] [--stroke F] [--seed N] [--stats FILE] "
	       "BACKING_DEV (use -q 1 -d 32)\n");
}

const struct ublk_tgt_ops hdd_tgt_ops = {
	.name = "hdd",
	.init_tgt = hdd_init_tgt,
	.deinit_tgt = hdd_deinit_tgt,
	.queue_io = hdd_queue_io,
	.tgt_io_done = hdd_io_done,
	.parse_cmd_line = hdd_cmd_line,
	.usage = hdd_usage,
};
