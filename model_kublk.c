// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Lumir Sliva
/*
 * Glue between a timing model and the kublk server, shared by the hdd and
 * ssd targets. Data goes to the backing device at once; the request
 * completes when both that I/O and the model's completion timer (an
 * absolute io_uring timeout) have finished.
 */

#include "model_kublk.h"
#include <math.h>
#include <time.h>

enum { MK_DATA = 0, MK_WAKE = 1, MK_DONE = 2 };

__u64 mk_now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (__u64)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static __u64 env_now(void *ctx)
{
	return mk_now();
}

static void set_ts(struct __kernel_timespec *ts, __u64 when)
{
	ts->tv_sec = when / 1000000000ULL;
	ts->tv_nsec = when % 1000000000ULL;
}

static void env_done(void *ctx, int tag, __u64 when)
{
	struct mk *k = ctx;
	struct io_uring_sqe *sqe;

	k->done_at[tag] = when;
	set_ts(&k->done_ts[tag], when);
	ublk_io_alloc_sqes(k->t, &sqe, 1);
	io_uring_prep_timeout(sqe, &k->done_ts[tag], 0, IORING_TIMEOUT_ABS);
	sqe->user_data = build_user_data(tag, 0, MK_DONE, k->q->q_id, 1);
}

/*
 * Arm a wake-up timer if `when` is earlier than every armed one. A slot's
 * timespec stays untouched until its CQE arrives, so it is still valid
 * whenever the ring gets submitted.
 */
static void env_wake(void *ctx, __u64 when)
{
	struct mk *k = ctx;
	struct io_uring_sqe *sqe;
	int slot;

	if (when >= k->timer_at || k->timer_busy == ~0ULL)
		return;
	slot = __builtin_ctzll(~k->timer_busy);
	k->timer_busy |= 1ULL << slot;
	k->timer_when[slot] = when;
	k->timer_at = when;
	set_ts(&k->timer_ts[slot], when);
	ublk_io_alloc_sqes(k->t, &sqe, 1);
	io_uring_prep_timeout(sqe, &k->timer_ts[slot], 0, IORING_TIMEOUT_ABS);
	sqe->user_data = build_user_data(slot, 0, MK_WAKE, k->q->q_id, 1);
}

static void wake_fired(struct mk *k, int slot)
{
	int i;

	k->timer_busy &= ~(1ULL << slot);
	k->timer_at = ~0ULL;
	for (i = 0; i < MK_TIMERS; i++)
		if ((k->timer_busy >> i & 1) && k->timer_when[i] < k->timer_at)
			k->timer_at = k->timer_when[i];
}

static void late_add(struct mk *k, __u64 late_ns)
{
	int b = (int)(MK_LATE_PER_OCTAVE * log2(1 + late_ns / 1e3));

	if (b >= MK_LATE_BUCKETS)
		b = MK_LATE_BUCKETS - 1;
	k->late_hist[b]++;
	k->late_n++;
	if (late_ns > k->late_max_ns)
		k->late_max_ns = late_ns;
}

/* upper edge, µs, of the bucket holding quantile q (at most ~4.4% high) */
static double late_quantile_us(const struct mk *k, double q)
{
	__u64 want = (__u64)ceil(q * k->late_n), seen = 0;
	int b;

	for (b = 0; b < MK_LATE_BUCKETS; b++) {
		seen += k->late_hist[b];
		if (seen >= want && seen)
			return pow(2, (b + 1) / (double)MK_LATE_PER_OCTAVE) - 1;
	}
	return 0;
}

void mk_init(struct mk *k, const char *stats_path)
{
	memset(k, 0, sizeof(*k));
	k->env.ctx = k;
	k->env.now = env_now;
	k->env.done = env_done;
	k->env.wake = env_wake;
	k->timer_at = ~0ULL;
	k->stats_fd = -1;
	if (stats_path && stats_path[0]) {
		k->stats_fd = open(stats_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (k->stats_fd < 0)
			ublk_err("can't open stats file %s: %m\n", stats_path);
	}
}

void mk_close(struct mk *k)
{
	if (k->stats_fd >= 0)
		close(k->stats_fd);
	k->stats_fd = -1;
}

unsigned mk_ring_depth(unsigned depth)
{
	/* per request: data io + completion timer, plus wake-up timers */
	return 4 * depth + MK_TIMERS + 8;
}

int mk_queue_io(struct mk *k, struct ublk_thread *t, struct ublk_queue *q,
		int tag, int *op, __u64 *lba, __u64 *nr)
{
	const struct ublksrv_io_desc *iod = ublk_get_iod(q, tag);
	unsigned uop = ublksrv_get_op(iod);
	struct io_uring_sqe *sqe;

	k->t = t;
	k->q = q;
	*lba = iod->start_sector;
	*nr = iod->nr_sectors;

	switch (uop) {
	case UBLK_IO_OP_READ:
	case UBLK_IO_OP_WRITE:
		ublk_io_alloc_sqes(t, &sqe, 1);
		io_uring_prep_rw(uop == UBLK_IO_OP_READ ? IORING_OP_READ :
				 IORING_OP_WRITE, sqe,
				 ublk_get_registered_fd(q, 1),
				 (void *)iod->addr, iod->nr_sectors << 9,
				 iod->start_sector << 9);
		io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
		sqe->user_data = build_user_data(tag, uop, MK_DATA, q->q_id, 1);
		ublk_queued_tgt_io(t, q, tag, 2);
		*op = uop == UBLK_IO_OP_READ ? MODEL_READ : MODEL_WRITE;
		return 0;
	case UBLK_IO_OP_FLUSH:
		ublk_queued_tgt_io(t, q, tag, 1);
		*op = MODEL_FLUSH;
		return 0;
	default:
		ublk_queued_tgt_io(t, q, tag, -EOPNOTSUPP);
		return -1;
	}
}

int mk_io_done(struct mk *k, struct ublk_thread *t, struct ublk_queue *q,
	       const struct io_uring_cqe *cqe)
{
	unsigned td = user_data_to_tgt_data(cqe->user_data);
	unsigned tag = user_data_to_tag(cqe->user_data);
	struct ublk_io *io;

	k->t = t;
	k->q = q;
	if (td == MK_WAKE) {
		wake_fired(k, tag);
		return 1;
	}

	io = ublk_get_io(q, tag);
	if (td == MK_DATA) {
		if (cqe->res < 0 || !io->result)
			io->result = cqe->res;
		if (cqe->res < 0)
			ublk_err("%s: data io failed tag %u res %d\n",
				 __func__, tag, cqe->res);
	} else {
		__u64 now = mk_now();

		if (cqe->res != -ETIME)
			ublk_err("%s: timer tag %u res %d\n", __func__, tag,
				 cqe->res);
		late_add(k, now > k->done_at[tag] ? now - k->done_at[tag] : 0);
	}

	if (ublk_completed_tgt_io(t, q, tag))
		ublk_complete_io(t, q, tag, io->result);
	return 0;
}

int mk_stats_due(struct mk *k)
{
	__u64 now = mk_now();

	if (k->stats_fd < 0 || now - k->last_stats < 1000000000ULL)
		return 0;
	k->last_stats = now;
	return 1;
}

void mk_stats_write(struct mk *k, const char *model)
{
	char buf[2048];
	int n;

	if (k->stats_fd < 0)
		return;
	n = snprintf(buf, sizeof(buf),
		"%scompletions %llu\nlate_us_p50 %.1f\nlate_us_p99 %.1f\n"
		"late_us_p999 %.1f\nlate_us_max %.1f\n", model,
		(unsigned long long)k->late_n, late_quantile_us(k, 0.5),
		late_quantile_us(k, 0.99), late_quantile_us(k, 0.999),
		k->late_max_ns / 1e3);
	if (n < 0)
		return;
	if (n >= (int)sizeof(buf))
		n = sizeof(buf) - 1;
	if (pwrite(k->stats_fd, buf, n, 0) == n &&
	    ftruncate(k->stats_fd, n) < 0)
		mk_close(k);
}
