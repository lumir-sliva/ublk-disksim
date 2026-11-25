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
	/*
	 * Capacity of the drive the device stands for, 0 = the device's
	 * own size. A 4 GiB device has only ~2400 tracks of its own, so
	 * requests often share one and need no seek; with the capacity
	 * given, two requests share a track only as often as they would
	 * on the real drive.
	 */
	double		capacity_gb;
	double		wsettle_ms;	/* extra settle of a seek that ends in a write */
	unsigned	cache_ext;	/* most dirty extents the cache holds, 0 = no limit */
	/*
	 * log_ms + log_us > 0: a flush does not write the cache back. It
	 * appends the extents not yet logged to a log on the platters
	 * (log_ms once, log_us and the media transfer per extent); they
	 * stay in the cache until written back in place.
	 */
	double		log_ms, log_us;
	/* write-back waits this long without a command, unless the cache is full */
	double		wb_idle_ms;
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
	__u64 unl;			/* sectors of it no flush has logged */
};

/* all state is here so the tests can check invariants */
struct hdd_model {
	struct hdd_params p;
	struct model_env env;
	double seek_full_ms;
	double period_ns;		/* one revolution */
	double spt;			/* sectors per track, from media rate and rpm */
	double tspt;			/* device sectors per track of the drive stood for */
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
	int destage_ext;		/* it took its extent out of `dirty` */
	__u64 destage_unl;		/* unlogged sectors it is putting in place */
	int destage_unl_ext;		/* ... the last ones of their extent */
	__u64 next_seq;			/* arrival counter for hdd_ext.seq */
	int wb_turn;			/* a write-back ran since the last queued request */
	int log;			/* flushes log instead of writing back */
	int logging;			/* the flush in progress is writing the log */
	/* neither logged nor in place: extents, and the write-back's share */
	int unlogged;
	__u64 unlogged_bytes;
	__u64 wb_idle_ns;
	__u64 last_cmd;			/* arrival of the latest command */
	__u64 last_nr;			/* size of the latest write cached */
	__u64 last_wend;		/* where the latest write to arrive ended */
	__u64 full_at;			/* when that write left no room for another */

	/* stats */
	__u64 n_read, n_write, n_flush, n_destage, n_cache_full, n_logged;
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
