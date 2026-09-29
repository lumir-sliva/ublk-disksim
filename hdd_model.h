/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Lumir Sliva */
/* hdd timing model, see hdd_model.c; kublk target glue in hdd.c */
#ifndef HDD_MODEL_H
#define HDD_MODEL_H

#include "model.h"

struct hdd_params {
	unsigned	rpm;
	double		seek_min_ms;	/* track to track */
	double		seek_avg_ms;	/* random, uniform over the stroke */
	double		mbps;		/* media transfer */
	double		iface_mbps;	/* host link (cached writes) */
	double		iface_us;	/* per-command overhead (cached writes) */
	unsigned	cache_mb;	/* volatile write cache; 0 = write-through */
	unsigned	ncq;		/* requests considered for reordering */
	double		max_wait_ms;	/* a request passed over this long goes next */
	unsigned	wb_window;	/* write-back picks among the N oldest dirty extents, 0 = all */
	double		stroke;		/* fraction of the full stroke the device spans */
	unsigned long long seed;
	char		stats[256];	/* stats file, rewritten once a second */
};

struct hdd_req {
	int tag;
	__u8 op;			/* MODEL_* */
	__u64 lba, nr;			/* sectors */
	__u64 arrive;
};

struct hdd_ext {
	__u64 lba, nr;			/* sectors */
	__u64 added;			/* when it entered the cache */
	__u64 seq;			/* arrival order (oldest merged piece) */
};

/* all state is here so the tests can check invariants */
struct hdd_model {
	struct hdd_params p;
	struct model_env env;
	double seek_full_ms;
	double period_ns;		/* one revolution */
	double spt;			/* sectors per track, from media rate and rpm */
	double phase0;			/* platter phase at t = 0, from the seed */
	__u64 max_wait_ns;		/* 0 = no age limit */
	__u64 dev_sectors;
	__u64 cache_bytes;
	unsigned depth;

	__u64 head;
	__u64 busy_until;

	/* waiting for the actuator: reads, write-through writes */
	struct hdd_req *pend;
	int npend;
	/* writes waiting for cache space, FIFO */
	struct hdd_req *wait;
	int nwait;
	/* arrived while a flush was in progress, FIFO */
	struct hdd_req *blocked, *spare;	/* spare: swapped in on replay */
	int nblocked;

	int flush_tag;
	__u64 flush_start;

	struct hdd_ext *dirty;
	int ndirty, cap_dirty;
	__u64 dirty_bytes;		/* extents + the write-back in progress */
	__u64 destaging;		/* bytes of the write-back in progress */
	__u64 next_seq;			/* arrival counter for hdd_ext.seq */
	int wb_turn;			/* a write-back ran since the last queued request */

	/* stats */
	__u64 n_read, n_write, n_flush, n_destage, n_cache_full;
	__u64 flush_ns_sum, flush_ns_max, flush_bytes_sum;
	__u64 n_blocked, blocked_ns_sum;
};

/* named parameter set; NULL name = the default profile; NULL if unknown */
const struct hdd_params *hdd_profile(const char *name);
/* 0 if usable with requests up to max_io_bytes, else -EINVAL */
int hdd_params_check(const struct hdd_params *p, __u64 max_io_bytes);
struct hdd_model *hdd_model_new(const struct hdd_params *p, __u64 dev_sectors,
				unsigned depth, const struct model_env *env);
void hdd_model_free(struct hdd_model *m);
/* a request arrives now; tags are < depth and unique while in flight */
void hdd_model_submit(struct hdd_model *m, int tag, int op, __u64 lba,
		      __u64 nr);
/* the wake-up the model asked for is due */
void hdd_model_wake(struct hdd_model *m);
/* counters as "name value" lines */
int hdd_model_stats(const struct hdd_model *m, char *buf, int len);

#endif
