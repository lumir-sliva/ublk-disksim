/* SPDX-License-Identifier: GPL-2.0 */
/* ssd timing model, see ssd_model.c; kublk target glue in ssd.c */
#ifndef SSD_MODEL_H
#define SSD_MODEL_H

#include "model.h"

#define SSD_NSTREAMS	8

struct ssd_params {
	unsigned	nvme;		/* 0: SATA (non-queued FLUSH), 1: NVMe */
	double		iface_mbps;	/* host link */
	double		cmd_us;		/* link time per command */
	double		iface_us;	/* controller latency per command */
	unsigned	dies;
	unsigned	page_kb;	/* program unit */
	double		tr_us;		/* page read, incl. ECC and lookup */
	double		ch_mbps;	/* flash channel */
	double		tprog_us;	/* page program */
	double		waf;		/* program units per random-write page */
	unsigned	buf_mb;		/* write buffer */
	unsigned	plp;		/* buffer survives power loss */
	unsigned	vwc;		/* advertise a volatile write cache */
	double		flush_us;	/* FLUSH cost once drained */
	double		floor_us;	/* host overhead to subtract */
	char		stats[256];	/* stats file, rewritten once a second */
};

struct ssd_req {
	int tag;
	__u8 op;			/* MODEL_* */
	__u8 rnd;			/* random write, costs waf units per page */
	__u64 lba, nr;			/* sectors */
	__u64 arrive;			/* reached the model's host side */
	__u64 t;			/* reached the model (later if held by a flush) */
	__u64 link_end;			/* write: data is on the drive */
};

/* a closed buffer page waiting to be programmed or being programmed */
struct ssd_page {
	__u64 bytes;
	__u64 ready;			/* closed at */
	double units;			/* program units not started yet */
	__u64 end;			/* end of its last unit */
	int freed;
};

struct ssd_iv {
	__u64 start, end;
};

struct ssd_flush {
	struct ssd_req r;
	__u64 seq;			/* done once pages up to seq are programmed */
};

/* all state is here so the tests can check invariants */
struct ssd_model {
	struct ssd_params p;
	struct model_env env;
	__u64 page_bytes, buf_cap;
	__u64 tr_ns, cmd_ns, iface_ns, flush_ns, floor_ns, unit_ns;
	unsigned depth;
	__u64 now;

	__u64 *die_free;

	/* host link busy intervals, sorted, non-overlapping */
	struct ssd_iv *link;
	int nlink, cap_link;

	/* write buffer: open page + closed pages [seq_head, seq_next) */
	__u64 buf_bytes;
	__u64 open_bytes, open_rnd, open_last;
	struct ssd_page *pg;
	__u64 cap_pg;			/* power of two */
	__u64 seq_head;			/* oldest page not freed */
	__u64 seq_start;		/* oldest page with units not started */
	__u64 seq_next;
	__u64 max_end_started;		/* latest end over fully started pages */
	__u64 next_unit;		/* when the next unit can start, if any */
	__u64 *heap;			/* fully started pages, min-heap on end */
	__u64 nheap;

	/* write stream ends, for sequential detection */
	__u64 stream_end[SSD_NSTREAMS], stream_use[SSD_NSTREAMS], stream_clock;

	struct ssd_req *wait;		/* writes waiting for buffer space, FIFO */
	int nwait;

	/* SATA: the one pending non-queued flush and what it holds back */
	int sflush_tag;
	struct ssd_req sflush;
	int sflush_closed;
	int dirty;			/* data admitted since the last flush took it */
	__u64 sflush_seq;
	struct ssd_req *held, *spare;	/* spare: swapped in on replay */
	int nheld;
	__u64 block_until;
	__u64 last_done;

	/* NVMe with plp = 0: flushes waiting for pages, FIFO */
	struct ssd_flush *nflush;
	int nnflush;

	/* stats */
	__u64 n_read, n_write, n_flush, n_buf_full, n_blocked, n_read_wait;
	__u64 seq_bytes, rnd_bytes;
	double units_done;
	__u64 flush_ns_sum, flush_ns_max, blocked_ns_sum, read_wait_ns_sum;
};

/* named parameter set; NULL name = the default profile; NULL if unknown */
const struct ssd_params *ssd_profile(const char *name);
/*
 * 0 if usable with requests up to max_io_bytes and `depth` outstanding
 * (SATA NCQ holds 32), else -EINVAL
 */
int ssd_params_check(const struct ssd_params *p, __u64 max_io_bytes,
		     unsigned depth);
struct ssd_model *ssd_model_new(const struct ssd_params *p, unsigned depth,
				const struct model_env *env);
void ssd_model_free(struct ssd_model *m);
/* a request arrives now; tags are < depth and unique while in flight */
void ssd_model_submit(struct ssd_model *m, int tag, int op, __u64 lba,
		      __u64 nr);
/* the wake-up the model asked for is due */
void ssd_model_wake(struct ssd_model *m);
/* counters as "name value" lines */
int ssd_model_stats(const struct ssd_model *m, char *buf, int len);

#endif
