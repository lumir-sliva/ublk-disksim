/* SPDX-License-Identifier: GPL-2.0-only */
/* SPDX-FileCopyrightText: 2026 Lumir Sliva */
/*
 * Glue between a timing model and the kublk server: data I/O to the
 * backing device, completion timers, wake-up timers, the stats file, and
 * a record of how late completions fire against the model's schedule.
 */
#ifndef MODEL_KUBLK_H
#define MODEL_KUBLK_H

#include "kublk.h"
#include "model.h"

#define MK_TIMERS	64
#define MK_LATE_PER_OCTAVE 16
#define MK_LATE_BUCKETS	(24 * MK_LATE_PER_OCTAVE)	/* 1/16 octaves of µs, to ~16 s */

struct mk {
	struct ublk_thread *t;
	struct ublk_queue *q;
	struct model_env env;

	/* completion timer per tag, and when it was meant to fire */
	struct __kernel_timespec done_ts[UBLK_QUEUE_DEPTH];
	__u64 done_at[UBLK_QUEUE_DEPTH];

	/* wake-up timers: a slot each, the earliest armed one in timer_at */
	__u64 timer_busy;
	__u64 timer_when[MK_TIMERS];
	__u64 timer_at;
	struct __kernel_timespec timer_ts[MK_TIMERS];

	/* completion lateness: actual - scheduled */
	__u64 late_hist[MK_LATE_BUCKETS];
	__u64 late_n, late_max_ns;

	int stats_fd;
	__u64 last_stats;
};

__u64 mk_now(void);
void mk_init(struct mk *k, const char *stats_path);
void mk_close(struct mk *k);
/* sq/cq entries a target with this queue depth needs */
unsigned mk_ring_depth(unsigned depth);
/*
 * Start the data I/O of a new request. Returns 0 and fills op/lba/nr if
 * the model should see it, -1 if it was completed here (unsupported op).
 */
int mk_queue_io(struct mk *k, struct ublk_thread *t, struct ublk_queue *q,
		int tag, int *op, __u64 *lba, __u64 *nr);
/* handle a target CQE; returns 1 if a wake-up timer fired */
int mk_io_done(struct mk *k, struct ublk_thread *t, struct ublk_queue *q,
	       const struct io_uring_cqe *cqe);
/* stats file: due at most once a second; `model` is the model's text */
int mk_stats_due(struct mk *k);
void mk_stats_write(struct mk *k, const char *model);

#endif
