// SPDX-License-Identifier: GPL-2.0
/*
 * Model tests: the hdd and ssd models on a virtual clock, without ublk.
 *
 * A small discrete-event simulator stands in for the kublk glue: it
 * provides struct model_env (now, done, wake), submits requests, delivers
 * completions at the time the model set, and wakes the model when asked.
 *
 * Scenario tests check documented behaviour against values derived from
 * the parameters and the physics (a sum of documented costs, a transfer
 * rate, a bound), never against a copy of the model's own formulas.
 * Randomized tests run random parameters and workloads and check
 * invariants on every event: each request completes once and never before
 * it arrived, nothing stalls, cache accounting adds up, a flush finds
 * everything before it on the media, no request waits past the age limit.
 *
 * Build and run: make check
 */

#include "hdd_model.h"
#include "ssd_model.h"
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXQ	128
#define MS	1000000ULL
#define US	1000ULL
#define SEC	1000000000ULL
#define NONE	(~0ULL)

static int failures, checks;
static const char *cur_test;

static void fail(const char *fmt, ...)
{
	va_list ap;

	if (failures++ < 50) {
		fprintf(stderr, "FAIL [%s] ", cur_test);
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
		fputc('\n', stderr);
	}
}

#define CHECK(cond, ...) do { checks++; if (!(cond)) fail(__VA_ARGS__); } while (0)

/* xorshift64*, deterministic across hosts */
static __u64 rng_state = 88172645463325252ULL;

static __u64 rnd(void)
{
	rng_state ^= rng_state >> 12;
	rng_state ^= rng_state << 25;
	rng_state ^= rng_state >> 27;
	return rng_state * 2685821657736338717ULL;
}

static __u64 rnd_below(__u64 n)
{
	return n ? rnd() % n : 0;
}

/* ---- simulator ---------------------------------------------------- */

struct job;

struct rq {
	int busy, has_done, op;
	__u64 lba, nr, sub, done_at, set_at;
	struct job *owner;
};

struct sim {
	__u64 now, wake_at;
	unsigned depth;
	struct hdd_model *h;
	struct ssd_model *s;
	struct rq rq[MAXQ];
	int inflight;
	/* randomized tests: bound on any read's latency, 0 = none */
	__u64 read_bound;
	/* check cache accounting after every model call (slow) */
	int deep_check;
};

static __u64 env_now(void *c)
{
	return ((struct sim *)c)->now;
}

static void env_wake(void *c, __u64 when)
{
	struct sim *s = c;

	if (when < s->wake_at)
		s->wake_at = when;
}

/*
 * At a flush's completion, everything written before it is on the media:
 * nothing left to write back, and the flush completes no earlier than the
 * last write-back or page program ends.
 */
static void flush_durable(struct sim *s, __u64 when)
{
	if (s->h && s->h->cache_bytes) {
		CHECK(!s->h->ndirty && !s->h->destaging && !s->h->nwait,
		      "hdd flush done with %d dirty extents, %llu bytes in "
		      "write-back, %d writes waiting", s->h->ndirty,
		      (unsigned long long)s->h->destaging, s->h->nwait);
		CHECK(when >= s->h->busy_until,
		      "hdd flush done %.3f ms before the last write-back ends",
		      (s->h->busy_until - when) / 1e6);
	}
	if (s->s && !s->s->p.nvme && !s->s->p.plp && s->s->p.vwc) {
		CHECK(s->s->seq_start == s->s->seq_next && !s->s->open_bytes,
		      "sata flush done with pages %llu..%llu not started or "
		      "%llu bytes unwritten", (unsigned long long)s->s->seq_start,
		      (unsigned long long)s->s->seq_next,
		      (unsigned long long)s->s->open_bytes);
		CHECK(when + s->s->floor_ns >= s->s->max_end_started,
		      "sata flush done %.3f us before its last page program ends",
		      (s->s->max_end_started - when - s->s->floor_ns) / 1e3);
	}
}

static void env_done(void *c, int tag, __u64 when)
{
	struct sim *s = c;
	struct rq *r;

	CHECK(tag >= 0 && tag < (int)s->depth, "done for tag %d", tag);
	if (tag < 0 || tag >= (int)s->depth)
		return;
	r = &s->rq[tag];
	CHECK(r->busy && !r->has_done, "done for tag %d not in flight or twice",
	      tag);
	CHECK(when >= r->sub, "tag %d completes %lld ns before it arrived",
	      tag, (long long)(r->sub - when));
	r->has_done = 1;
	r->done_at = when;
	r->set_at = s->now;
	if (r->op == MODEL_FLUSH)
		flush_durable(s, when);
}

/* model state that must always hold (randomized tests) */
static void check_state(struct sim *s)
{
	if (!s->deep_check)
		return;
	if (s->h) {
		struct hdd_model *m = s->h;
		__u64 sum = m->destaging;
		int i;

		/* sorted by LBA, disjoint, not touching: the model relies on it */
		for (i = 0; i < m->ndirty; i++) {
			sum += m->dirty[i].nr << 9;
			if (i)
				CHECK(m->dirty[i - 1].lba + m->dirty[i - 1].nr <
				      m->dirty[i].lba,
				      "dirty extents unsorted, overlapping or "
				      "touching at %llu",
				      (unsigned long long)m->dirty[i].lba);
		}
		CHECK(sum == m->dirty_bytes, "dirty_bytes %llu, extents sum %llu",
		      (unsigned long long)m->dirty_bytes,
		      (unsigned long long)sum);
		CHECK(!m->cache_bytes || m->dirty_bytes <= m->cache_bytes,
		      "dirty %llu over cache %llu",
		      (unsigned long long)m->dirty_bytes,
		      (unsigned long long)m->cache_bytes);
	}
	if (s->s) {
		struct ssd_model *m = s->s;
		__u64 sum = m->open_bytes, q;

		for (q = m->seq_head; q < m->seq_next; q++) {
			const struct ssd_page *pg = &m->pg[q & (m->cap_pg - 1)];

			if (!pg->freed)
				sum += pg->bytes;
		}
		CHECK(sum == m->buf_bytes, "buf_bytes %llu, pages sum %llu",
		      (unsigned long long)m->buf_bytes,
		      (unsigned long long)sum);
		CHECK(m->buf_bytes <= m->buf_cap, "buffer over capacity");
		CHECK(m->pool >= 0 && m->pool <= m->pool_cap * (1 + 1e-9),
		      "gc pool %.0f outside 0..%.0f", m->pool, m->pool_cap);
		for (q = 0; q < m->p.dies; q++)
			CHECK(m->die_free[q] == (m->prog_end[q] > m->read_end[q] ?
				m->prog_end[q] : m->read_end[q]),
			      "die %llu: free at %llu, program to %llu, reads to %llu",
			      (unsigned long long)q,
			      (unsigned long long)m->die_free[q],
			      (unsigned long long)m->prog_end[q],
			      (unsigned long long)m->read_end[q]);
	}
}

static void sim_init(struct sim *s, unsigned depth)
{
	memset(s, 0, sizeof(*s));
	s->depth = depth;
	s->wake_at = NONE;
}

static struct model_env sim_env(struct sim *s)
{
	struct model_env e = { s, env_now, env_done, env_wake };

	return e;
}

static void sim_free(struct sim *s)
{
	hdd_model_free(s->h);
	ssd_model_free(s->s);
	s->h = NULL;
	s->s = NULL;
}

/* submit at the current time; tag, or -1 if all tags are busy */
static int sim_submit(struct sim *s, int op, __u64 lba, __u64 nr,
		      struct job *owner)
{
	int tag;

	for (tag = 0; tag < (int)s->depth; tag++)
		if (!s->rq[tag].busy)
			break;
	if (tag == (int)s->depth)
		return -1;
	s->rq[tag] = (struct rq) { .busy = 1, .op = op, .lba = lba, .nr = nr,
				   .sub = s->now, .owner = owner };
	s->inflight++;
	if (s->h)
		hdd_model_submit(s->h, tag, op, lba, nr);
	else
		ssd_model_submit(s->s, tag, op, lba, nr);
	check_state(s);
	return tag;
}

static void job_done(struct sim *s, struct job *j, const struct rq *r);

/* process the next event at or before `limit`; 0 if there is none */
static int sim_step(struct sim *s, __u64 limit)
{
	__u64 t = NONE;
	int tag, which = -1;

	for (tag = 0; tag < (int)s->depth; tag++) {
		const struct rq *r = &s->rq[tag];
		__u64 at;

		if (!r->busy || !r->has_done)
			continue;
		at = r->done_at > r->set_at ? r->done_at : r->set_at;
		if (at < t) {
			t = at;
			which = tag;
		}
	}
	if (s->wake_at < t) {
		t = s->wake_at;
		which = -1;
	}
	if (t == NONE || t > limit)
		return 0;
	if (t > s->now)
		s->now = t;
	if (which < 0) {
		s->wake_at = NONE;
		if (s->h)
			hdd_model_wake(s->h);
		else
			ssd_model_wake(s->s);
		check_state(s);
	} else {
		struct rq r = s->rq[which];

		s->rq[which].busy = 0;
		s->inflight--;
		if (s->read_bound && r.op == MODEL_READ)
			CHECK(r.done_at - r.sub <= s->read_bound,
			      "read waited %.1f ms, bound %.1f ms",
			      (r.done_at - r.sub) / 1e6, s->read_bound / 1e6);
		if (r.owner)
			job_done(s, r.owner, &r);
	}
	return 1;
}

static void sim_run_until(struct sim *s, __u64 t)
{
	while (sim_step(s, t))
		;
	if (s->now < t)
		s->now = t;
}

/* run until nothing is in flight; a stall (work left, no event) fails */
static void sim_drain(struct sim *s, __u64 cap)
{
	while (s->inflight) {
		if (!sim_step(s, cap)) {
			CHECK(0, "%s: %d requests never complete (%s)",
			      "drain", s->inflight,
			      s->wake_at == NONE ? "no wake-up pending: stall"
						 : "time cap reached");
			break;
		}
	}
}

/* ---- closed-loop jobs --------------------------------------------- */

enum { RAND, SEQ };

struct job {
	int op, pattern, qd;
	__u64 nr, lo, hi, next;		/* sectors: size, span, next seq lba */
	__u64 stop_at, measure_from;
	__u64 n, bytes, lat_sum, lat_max;
	int flush_every, since_flush;
};

static void job_issue(struct sim *s, struct job *j)
{
	__u64 lba;

	if (j->flush_every && j->since_flush == j->flush_every) {
		j->since_flush = 0;
		sim_submit(s, MODEL_FLUSH, 0, 0, j);
		return;
	}
	if (j->pattern == SEQ) {
		if (j->next + j->nr > j->hi)
			j->next = j->lo;
		lba = j->next;
		j->next += j->nr;
	} else {
		lba = j->lo + rnd_below((j->hi - j->lo) / j->nr) * j->nr;
	}
	j->since_flush++;
	sim_submit(s, j->op, lba, j->nr, j);
}

static void job_done(struct sim *s, struct job *j, const struct rq *r)
{
	__u64 lat = r->done_at - r->sub;

	if (r->sub >= j->measure_from && r->op != MODEL_FLUSH) {
		j->n++;
		j->bytes += r->nr << 9;
		j->lat_sum += lat;
		if (lat > j->lat_max)
			j->lat_max = lat;
	}
	if (s->now < j->stop_at)
		job_issue(s, j);
}

static void job_start(struct sim *s, struct job *j)
{
	int i;

	j->next = j->lo;
	for (i = 0; i < j->qd; i++)
		job_issue(s, j);
}

static double job_iops(const struct job *j, __u64 secs_ns)
{
	return j->n / (secs_ns / 1e9);
}

static double job_mean_ms(const struct job *j)
{
	return j->n ? j->lat_sum / 1e6 / j->n : 0;
}

/* ---- hdd scenarios ------------------------------------------------ */

#define HDD_SECTORS (4ULL << 21)	/* 4 GiB */

static struct hdd_params hdd_p(void)
{
	return *hdd_profile("hgst-7k8");
}

static void hdd_up(struct sim *s, const struct hdd_params *p, unsigned depth)
{
	struct model_env e;

	sim_init(s, depth);
	e = sim_env(s);
	s->h = hdd_model_new(p, HDD_SECTORS, depth, &e);
}

/* one track in ns and sectors, from the documented media rate and rpm */
static double rev_ns(const struct hdd_params *p) { return 60e9 / p->rpm; }
static double xfer_ns_of(double mbps, __u64 bytes) { return bytes / (mbps * 1e6) * 1e9; }

static void hdd_seq_read_is_transfer(void)
{
	struct hdd_params p = hdd_p();
	struct job j = { .op = MODEL_READ, .pattern = SEQ, .qd = 1, .nr = 128,
			 .lo = 0, .hi = HDD_SECTORS, .stop_at = NONE };
	struct sim s;
	__u64 prev = 0;
	int i;

	cur_test = "hdd: sequential reads cost only the media transfer";
	hdd_up(&s, &p, 32);
	for (i = 0; i < 200; i++) {
		int tag = sim_submit(&s, MODEL_READ, i * j.nr, j.nr, NULL);
		__u64 done;

		sim_drain(&s, s.now + SEC);
		done = s.rq[tag].done_at;
		if (i > 0)
			CHECK(fabs((done - prev) - xfer_ns_of(p.mbps, j.nr << 9)) < 1000,
			      "read %d took %.1f us, media transfer %.1f us", i,
			      (done - prev) / 1e3,
			      xfer_ns_of(p.mbps, j.nr << 9) / 1e3);
		prev = done;
	}
	sim_free(&s);
}

static double hdd_random_qd(const struct hdd_params *p, int op, int qd,
			    __u64 secs, double *mean_ms)
{
	struct job j = { .op = op, .pattern = RAND, .qd = qd, .nr = 8,
			 .lo = 0, .hi = HDD_SECTORS, .stop_at = secs * SEC,
			 .measure_from = SEC };
	struct sim s;
	double iops;

	hdd_up(&s, p, 32);
	job_start(&s, &j);
	sim_run_until(&s, secs * SEC);
	sim_drain(&s, (secs + 30) * SEC);
	iops = job_iops(&j, (secs - 1) * SEC);
	if (mean_ms)
		*mean_ms = job_mean_ms(&j);
	sim_free(&s);
	return iops;
}

static void hdd_random_read_latency(void)
{
	struct hdd_params p = hdd_p();
	double mean, want, qd1, qd32;

	cur_test = "hdd: random read QD1 = average seek + half a revolution";
	qd1 = hdd_random_qd(&p, MODEL_READ, 1, 60, &mean);
	want = p.seek_avg_ms + rev_ns(&p) / 2e6 + xfer_ns_of(p.mbps, 4096) / 1e6;
	CHECK(fabs(mean - want) / want < 0.04,
	      "mean %.2f ms, seek_avg + rev/2 + transfer = %.2f ms", mean, want);

	cur_test = "hdd: NCQ reordering at QD32 (~200 IOPS for 7200 rpm)";
	qd32 = hdd_random_qd(&p, MODEL_READ, 32, 30, NULL);
	CHECK(qd32 > 180 && qd32 < 240 && qd32 > 2.2 * qd1,
	      "QD32 %.0f IOPS, QD1 %.0f IOPS", qd32, qd1);

	cur_test = "hdd: cache off, a random write costs what a read does";
	p.cache_mb = 0;
	hdd_random_qd(&p, MODEL_WRITE, 1, 30, &mean);
	CHECK(fabs(mean - want) / want < 0.05,
	      "write-through mean %.2f ms, expected %.2f ms", mean, want);
}

static void hdd_cached_write_and_flush(void)
{
	struct hdd_params p = hdd_p();
	double max_op = (p.seek_min_ms + (p.seek_avg_ms - p.seek_min_ms) * 15 / 8) * 1e6
		+ rev_ns(&p) + xfer_ns_of(p.mbps, 4096);
	struct sim s;
	int tag, f, r, i;
	__u64 t0;

	cur_test = "hdd: a cached write returns after the host transfer";
	hdd_up(&s, &p, 32);
	tag = sim_submit(&s, MODEL_WRITE, 12345 * 8, 8, NULL);
	CHECK(fabs((double)(s.rq[tag].done_at - s.rq[tag].sub) -
		   (p.iface_us * 1e3 + xfer_ns_of(p.iface_mbps, 4096))) < 2,
	      "cached write %.2f us, iface_us + 4K/iface_mbps = %.2f us",
	      (s.rq[tag].done_at - s.rq[tag].sub) / 1e3,
	      p.iface_us + xfer_ns_of(p.iface_mbps, 4096) / 1e3);
	sim_drain(&s, s.now + SEC);
	sim_run_until(&s, s.now + 2 * SEC);	/* written back while idle */
	CHECK(s.h->dirty_bytes == 0, "idle drive didn't write back its cache");
	sim_free(&s);

	/*
	 * 64K writes spaced apart (no merging) in the far half of the device,
	 * all at once: none can be on the platters before the head has moved
	 * there, at least a track-to-track seek, so until then the cache
	 * acknowledges exactly cache_mb of them, the one being written back
	 * included.
	 */
	cur_test = "hdd: before anything reaches the platters the cache takes cache_mb";
	p.cache_mb = 1;
	{
		unsigned fit = (p.cache_mb << 20) / 65536, n = fit + 8, fast = 0;
		int tags[MAXQ];
		__u64 horizon;

		hdd_up(&s, &p, n);
		horizon = s.now + (__u64)(p.seek_min_ms * 1e6);
		for (i = 0; i < (int)n; i++)
			tags[i] = sim_submit(&s, MODEL_WRITE,
					     HDD_SECTORS / 2 + i * 1024ULL, 128, NULL);
		sim_drain(&s, s.now + 10 * SEC);
		for (i = 0; i < (int)n; i++)
			fast += s.rq[tags[i]].done_at < horizon;
		CHECK(fast == fit, "%u 64K writes acknowledged before any "
		      "write-back could end, the cache holds %u", fast, fit);
		sim_free(&s);
	}
	p = hdd_p();

	cur_test = "hdd: FLUSH writes back everything and holds later requests";
	hdd_up(&s, &p, 32);
	for (i = 0; i < 16; i++) {
		sim_submit(&s, MODEL_WRITE, rnd_below(HDD_SECTORS / 8) * 8, 8, NULL);
		sim_drain(&s, s.now + SEC);
	}
	t0 = s.now;
	f = sim_submit(&s, MODEL_FLUSH, 0, 0, NULL);
	s.now += 1000;
	r = sim_submit(&s, MODEL_READ, 777 * 8, 8, NULL);
	sim_drain(&s, s.now + 10 * SEC);
	CHECK(s.rq[r].done_at >= s.rq[f].done_at,
	      "read finished %.3f ms before the flush it arrived behind",
	      (s.rq[f].done_at - s.rq[r].done_at) / 1e6);
	CHECK(s.rq[f].done_at - t0 <= 16 * max_op,
	      "flush of 16 writes took %.1f ms, over 16 worst-case ops %.1f ms",
	      (s.rq[f].done_at - t0) / 1e6, 16 * max_op / 1e6);
	sim_free(&s);

	cur_test = "hdd: cache off, FLUSH is free";
	p.cache_mb = 0;
	hdd_up(&s, &p, 32);
	f = sim_submit(&s, MODEL_FLUSH, 0, 0, NULL);
	CHECK(s.rq[f].done_at == s.rq[f].sub, "flush took %llu ns",
	      (unsigned long long)(s.rq[f].done_at - s.rq[f].sub));
	sim_free(&s);
}

/*
 * Flush after 64 cached 4K writes scattered over the device, all arriving
 * at once and followed at once by the flush, as a deferred batch is (a
 * queue deeper than SATA's 32 so they fit in one go).
 */
static double hdd_flush_after_64(const struct hdd_params *p)
{
	struct sim s;
	__u64 t0;
	int i, f;
	double t;

	hdd_up(&s, p, MAXQ);
	for (i = 0; i < 64; i++)
		CHECK(sim_submit(&s, MODEL_WRITE, rnd_below(HDD_SECTORS / 8) * 8,
				 8, NULL) >= 0, "write %d not submitted", i);
	t0 = s.now;
	f = sim_submit(&s, MODEL_FLUSH, 0, 0, NULL);
	CHECK(f >= 0, "flush not submitted");
	if (f < 0) {
		sim_free(&s);
		return 0;
	}
	sim_drain(&s, s.now + 60 * SEC);
	t = (double)(s.rq[f].done_at - t0);
	sim_free(&s);
	return t;
}

static void hdd_wb_window(void)
{
	struct hdd_params p = hdd_p();
	/* one write-back at a random position: average seek + half a turn */
	double one = p.seek_avg_ms * 1e6 + rev_ns(&p) / 2 + xfer_ns_of(p.mbps, 4096);
	double t0, t4, t1;
	__u64 saved = rng_state;

	/* the same 64 addresses each time: only the window differs */
	p.wb_window = 0;
	t0 = hdd_flush_after_64(&p);
	rng_state = saved;
	p.wb_window = 4;
	t4 = hdd_flush_after_64(&p);
	rng_state = saved;
	p.wb_window = 1;
	t1 = hdd_flush_after_64(&p);

	cur_test = "hdd: wb_window 1 writes back in arrival order, one random access each";
	CHECK(t1 > 0.7 * 64 * one && t1 < 1.3 * 64 * one,
	      "flush of 64 scattered writes %.0f ms, 64 random accesses = %.0f ms",
	      t1 / 1e6, 64 * one / 1e6);
	cur_test = "hdd: a wider write-back window reorders more";
	CHECK(t0 < t4 && t4 < t1, "flush after 64: window all %.0f, 4 %.0f, 1 %.0f ms",
	      t0 / 1e6, t4 / 1e6, t1 / 1e6);
}

static void hdd_writeback_by_track(void)
{
	struct hdd_params p = hdd_p();
	struct job w = { .op = MODEL_WRITE, .pattern = SEQ, .qd = 4, .nr = 2048,
			 .lo = 0, .hi = HDD_SECTORS / 2, .stop_at = 3 * SEC };
	double full_seek = (p.seek_min_ms + (p.seek_avg_ms - p.seek_min_ms) * 15 / 8) * 1e6;
	double bound = 2 * rev_ns(&p) + full_seek + rev_ns(&p) +
		xfer_ns_of(p.mbps, 4096) + 1e6;
	struct job probe = { 0 };
	struct sim s;

	cur_test = "hdd: a read waits for at most a track of write-back";
	hdd_up(&s, &p, 32);
	job_start(&s, &w);
	sim_run_until(&s, 2 * SEC);		/* cache full, streaming */
	CHECK(s.h->dirty_bytes >= s.h->cache_bytes / 4 * 3,
	      "cache not under pressure: the test doesn't exercise write-back");
	sim_submit(&s, MODEL_READ, HDD_SECTORS - 8, 8, &probe);
	sim_run_until(&s, 3 * SEC);
	CHECK(probe.n == 1 && probe.lat_max <= bound,
	      "read waited %.1f ms, bound (two tracks + full seek + rev) %.1f ms",
	      probe.lat_max / 1e6, bound / 1e6);
	sim_drain(&s, 60 * SEC);
	sim_free(&s);
}

static void hdd_writer_next_to_readers(void)
{
	struct hdd_params p = hdd_p();
	struct job rd = { .op = MODEL_READ, .pattern = RAND, .qd = 4, .nr = 8,
			  .lo = 0, .hi = HDD_SECTORS, .stop_at = 30 * SEC };
	struct job wr = { .op = MODEL_WRITE, .pattern = RAND, .qd = 1, .nr = 8,
			  .lo = 0, .hi = HDD_SECTORS, .stop_at = 30 * SEC,
			  .measure_from = 5 * SEC };
	struct sim s;

	cur_test = "hdd: a cached writer next to QD4 readers keeps moving";
	hdd_up(&s, &p, 32);
	job_start(&s, &rd);
	job_start(&s, &wr);
	sim_run_until(&s, 30 * SEC);
	/* the cache is full after ~1 s; after that writes move at the pace
	 * of write-back, one per queued read served (~50/s each at QD4) */
	CHECK(s.h->nwait || s.h->dirty_bytes >= s.h->cache_bytes / 4 * 3,
	      "cache not under pressure: the test doesn't exercise it");
	CHECK(job_iops(&wr, 25 * SEC) > 30,
	      "with a full cache the writer moved %.1f writes/s", job_iops(&wr, 25 * SEC));
	CHECK(wr.lat_max < SEC, "a write waited %.0f ms", wr.lat_max / 1e6);
	CHECK(rd.n > 30 * 50, "readers got %llu reads in 30 s",
	      (unsigned long long)rd.n);
	sim_drain(&s, 120 * SEC);
	sim_free(&s);
}

static void hdd_age_limit(void)
{
	struct hdd_params p = hdd_p();
	double full_seek = (p.seek_min_ms + (p.seek_avg_ms - p.seek_min_ms) * 15 / 8) * 1e6;
	double op = full_seek + rev_ns(&p) + xfer_ns_of(p.mbps, 1 << 20);
	int limit;

	p.cache_mb = 0;
	for (limit = 1; limit >= 0; limit--) {
		struct job w = { .op = MODEL_WRITE, .pattern = SEQ, .qd = 4,
				 .nr = 2048, .lo = 0, .hi = HDD_SECTORS / 2,
				 .stop_at = 5 * SEC };
		struct job probe = { 0 };
		struct sim s;

		if (!limit)
			p.max_wait_ms = 0;
		cur_test = limit ?
			"hdd: a read behind a sequential stream waits max_wait_ms at most" :
			"hdd: without the age limit the stream starves the read";
		hdd_up(&s, &p, 32);
		job_start(&s, &w);
		sim_run_until(&s, SEC);
		sim_submit(&s, MODEL_READ, HDD_SECTORS - 8, 8, &probe);
		sim_run_until(&s, 5 * SEC);
		sim_drain(&s, 60 * SEC);
		if (limit)
			CHECK(probe.n == 1 &&
			      probe.lat_max <= p.max_wait_ms * 1e6 + 3 * op,
			      "read waited %.0f ms, limit 500 ms + 3 ops",
			      probe.lat_max / 1e6);
		else
			CHECK(probe.lat_max > 3 * SEC,
			      "read done after %.0f ms without an age limit: the "
			      "test doesn't exercise starvation", probe.lat_max / 1e6);
		sim_free(&s);
	}
}

/* ---- ssd scenarios ------------------------------------------------ */

#define SSD_SECTORS (4ULL << 21)	/* 4 GiB */

static void ssd_up(struct sim *s, const struct ssd_params *p, unsigned depth)
{
	struct model_env e;

	sim_init(s, depth);
	e = sim_env(s);
	s->s = ssd_model_new(p, SSD_SECTORS, depth, &e);
}

/* documented cost of a QD1 4K read and write, ns */
static double ssd_read_ns(const struct ssd_params *p)
{
	return (p->tr_us + p->cmd_us + p->iface_us) * 1e3 +
		xfer_ns_of(p->ch_mbps, 4096) + xfer_ns_of(p->iface_mbps, 4096);
}

static double ssd_unit_ns(const struct ssd_params *p)
{
	return p->tprog_us * 1e3 + xfer_ns_of(p->ch_mbps, p->page_kb << 10);
}

static void ssd_qd1_costs(void)
{
	struct ssd_params p = *ssd_profile("sata-plp");
	struct sim s;
	int tag;

	cur_test = "ssd: QD1 read = tr + channel + command + link + controller";
	ssd_up(&s, &p, 32);
	tag = sim_submit(&s, MODEL_READ, 1000 * 8, 8, NULL);
	CHECK(fabs((double)(s.rq[tag].done_at - s.rq[tag].sub) - ssd_read_ns(&p)) < 5,
	      "read %.3f us, documented sum %.3f us",
	      (s.rq[tag].done_at - s.rq[tag].sub) / 1e3, ssd_read_ns(&p) / 1e3);
	sim_drain(&s, SEC);

	cur_test = "ssd: QD1 write = command + link + controller";
	tag = sim_submit(&s, MODEL_WRITE, 2000 * 8, 8, NULL);
	CHECK(fabs((double)(s.rq[tag].done_at - s.rq[tag].sub) -
		   ((p.cmd_us + p.iface_us) * 1e3 + xfer_ns_of(p.iface_mbps, 4096))) < 5,
	      "write %.3f us", (s.rq[tag].done_at - s.rq[tag].sub) / 1e3);
	sim_drain(&s, SEC);
	sim_free(&s);

	cur_test = "ssd: floor_us is taken off every completion, never below arrival";
	p.floor_us = 50;
	ssd_up(&s, &p, 32);
	tag = sim_submit(&s, MODEL_READ, 1000 * 8, 8, NULL);
	CHECK(fabs((double)(s.rq[tag].done_at - s.rq[tag].sub) -
		   (ssd_read_ns(&p) - 50e3)) < 5,
	      "read with floor 50 us: %.3f us", (s.rq[tag].done_at - s.rq[tag].sub) / 1e3);
	sim_drain(&s, SEC);
	sim_free(&s);
	p.floor_us = 500;
	ssd_up(&s, &p, 32);
	tag = sim_submit(&s, MODEL_READ, 1000 * 8, 8, NULL);
	CHECK(s.rq[tag].done_at == s.rq[tag].sub, "floor above the device time");
	sim_drain(&s, SEC);
	sim_free(&s);
}

static void ssd_die_parallelism(void)
{
	struct ssd_params p = *ssd_profile("nvme-plp");
	__u64 page = p.page_kb * 2, t0, last = 0;
	struct sim s;
	unsigned i;
	int tags[MAXQ];

	cur_test = "ssd: reads on different dies overlap";
	ssd_up(&s, &p, 128);
	t0 = s.now;
	for (i = 0; i < p.dies; i++)
		tags[i] = sim_submit(&s, MODEL_READ, i * page, 8, NULL);
	for (i = 0; i < p.dies; i++)
		if (s.rq[tags[i]].done_at > last)
			last = s.rq[tags[i]].done_at;
	CHECK(last - t0 < 2 * ssd_read_ns(&p) + p.dies * xfer_ns_of(p.iface_mbps, 4096),
	      "%u reads on %u dies took %.1f us", p.dies, p.dies, (last - t0) / 1e3);
	sim_drain(&s, SEC);
	sim_free(&s);

	cur_test = "ssd: reads on one die queue behind each other";
	ssd_up(&s, &p, 128);
	t0 = s.now;
	last = 0;
	for (i = 0; i < 32; i++)
		tags[i] = sim_submit(&s, MODEL_READ, i * page * p.dies, 8, NULL);
	for (i = 0; i < 32; i++)
		if (s.rq[tags[i]].done_at > last)
			last = s.rq[tags[i]].done_at;
	CHECK(last - t0 >= 32 * p.tr_us * 1e3,
	      "32 reads on one die took %.1f us, 32 x tr = %.1f us",
	      (last - t0) / 1e3, 32 * p.tr_us);
	sim_drain(&s, SEC);
	sim_free(&s);
}

static double ssd_run(const struct ssd_params *p, unsigned depth, int op,
		      int pattern, __u64 nr, int qd, __u64 warm, __u64 secs,
		      double *mbps)
{
	struct job j = { .op = op, .pattern = pattern, .qd = qd, .nr = nr,
			 .lo = 0, .hi = SSD_SECTORS, .stop_at = (warm + secs) * SEC,
			 .measure_from = warm * SEC };
	struct sim s;
	double iops;

	ssd_up(&s, p, depth);
	job_start(&s, &j);
	sim_run_until(&s, (warm + secs) * SEC);
	iops = job_iops(&j, secs * SEC);
	if (mbps)
		*mbps = j.bytes / 1e6 / secs;
	sim_drain(&s, (warm + secs + 60) * SEC);
	sim_free(&s);
	return iops;
}

static void ssd_rates(void)
{
	struct ssd_params p = *ssd_profile("sata-plp");
	double iops, want, mbps, link, prog;

	cur_test = "ssd: SATA QD32 random reads run at the command rate";
	iops = ssd_run(&p, 32, MODEL_READ, RAND, 8, 32, 0, 1, NULL);
	want = 1e9 / (p.cmd_us * 1e3 + xfer_ns_of(p.iface_mbps, 4096));
	CHECK(fabs(iops - want) / want < 0.03, "%.0f IOPS, 1/(cmd + 4K link) = %.0f",
	      iops, want);

	cur_test = "ssd: NVMe reads with cmd_us top out at the command rate";
	p = *ssd_profile("nvme-plp");
	p.cmd_us = 2;
	iops = ssd_run(&p, 64, MODEL_READ, RAND, 8, 64, 0, 1, NULL);
	want = 1e9 / (p.cmd_us * 1e3 + xfer_ns_of(p.iface_mbps, 4096));
	CHECK(fabs(iops - want) / want < 0.03, "%.0f IOPS, 1/(cmd + 4K link) = %.0f",
	      iops, want);
	p = *ssd_profile("sata-plp");

	cur_test = "ssd: steady random writes = dies x page / (unit x waf)";
	iops = ssd_run(&p, 32, MODEL_WRITE, RAND, 8, 32, 3, 10, NULL);
	want = p.dies * (p.page_kb << 10) / (ssd_unit_ns(&p) * p.waf / 1e9) / 4096;
	CHECK(fabs(iops - want) / want < 0.05, "%.0f IOPS, expected %.0f", iops, want);

	cur_test = "ssd: sequential writes = min(link, dies x page / unit)";
	ssd_run(&p, 32, MODEL_WRITE, SEQ, 256, 32, 1, 3, &mbps);
	link = (256 << 9) / ((p.cmd_us * 1e3 + xfer_ns_of(p.iface_mbps, 256 << 9)) / 1e9) / 1e6;
	prog = p.dies * (p.page_kb << 10) / (ssd_unit_ns(&p) / 1e9) / 1e6;
	want = link < prog ? link : prog;
	CHECK(fabs(mbps - want) / want < 0.05, "%.0f MB/s, expected %.0f", mbps, want);

	cur_test = "ssd: NVMe sequential writes limited by programming";
	p = *ssd_profile("nvme-plp");
	ssd_run(&p, 128, MODEL_WRITE, SEQ, 256, 32, 1, 3, &mbps);
	want = p.dies * (p.page_kb << 10) / (ssd_unit_ns(&p) / 1e9) / 1e6;
	CHECK(fabs(mbps - want) / want < 0.05, "%.0f MB/s, expected %.0f", mbps, want);
}

/* MB/s of 128K sequential writes at QD32 over [lo, hi) for secs, on s */
static double ssd_seq_write_mbps(struct sim *s, __u64 lo, __u64 hi, int secs)
{
	struct job j = { .op = MODEL_WRITE, .pattern = SEQ, .qd = 32, .nr = 256,
			 .lo = lo, .hi = hi };

	j.measure_from = s->now + SEC / 2;
	j.stop_at = j.measure_from + secs * SEC;
	job_start(s, &j);
	sim_run_until(s, j.stop_at);
	sim_drain(s, s->now + 60 * SEC);
	return j.bytes / 1e6 / secs;
}

static void ssd_seq_over_scattered(void)
{
	struct ssd_params p = *ssd_profile("nvme-plp");
	double prog, want, first, mbps, g;
	__u64 range = 64ULL << 11;	/* 64 MiB */
	struct sim s;

	/* program rate of sequential writes paying waf on a share g */
	prog = p.dies * (p.page_kb << 10) / (ssd_unit_ns(&p) / 1e9) / 1e6;
	p.history = 1;
	ssd_up(&s, &p, 128);

	cur_test = "ssd: on a scattered drive sequential writes pay waf";
	g = 1 - (double)range / SSD_SECTORS;	/* after one pass over the range */
	first = ssd_seq_write_mbps(&s, 0, range, 2);
	want = prog / (1 + (p.waf - 1) * g);
	CHECK(fabs(first - want) / want < 0.05, "%.0f MB/s, expected dies x page "
	      "/ (unit x (1 + (waf - 1) x %.3f)) = %.0f", first, g, want);

	cur_test = "ssd: rewriting a range doesn't make it cheap: the drive's share counts";
	mbps = ssd_seq_write_mbps(&s, 0, range, 2);
	CHECK(fabs(mbps - first) / first < 0.03, "second pass %.0f MB/s, first %.0f",
	      mbps, first);

	cur_test = "ssd: half the drive rewritten sequentially: they pay half";
	ssd_seq_write_mbps(&s, SSD_SECTORS / 2, SSD_SECTORS, 8);
	mbps = ssd_seq_write_mbps(&s, 0, range, 2);
	want = prog / (1 + (p.waf - 1) * (0.5 - (double)range / SSD_SECTORS));
	CHECK(fabs(mbps - want) / want < 0.05, "%.0f MB/s, expected %.0f", mbps,
	      want);
	sim_free(&s);
}

static void ssd_flushes(void)
{
	struct ssd_params p = *ssd_profile("sata-plp");
	struct sim s;
	int w, f, r;
	__u64 t;

	cur_test = "ssd: with PLP a flush costs flush_us";
	ssd_up(&s, &p, 32);
	sim_submit(&s, MODEL_WRITE, 8 * 999, 8, NULL);
	sim_drain(&s, SEC);
	f = sim_submit(&s, MODEL_FLUSH, 0, 0, NULL);
	sim_drain(&s, s.now + SEC);
	CHECK(fabs((double)(s.rq[f].done_at - s.rq[f].sub) - p.flush_us * 1e3) < 5,
	      "flush %.3f us, flush_us %.1f", (s.rq[f].done_at - s.rq[f].sub) / 1e3,
	      p.flush_us);
	cur_test = "ssd: a flush with nothing written since the last is free";
	f = sim_submit(&s, MODEL_FLUSH, 0, 0, NULL);
	sim_drain(&s, s.now + SEC);
	CHECK(s.rq[f].done_at == s.rq[f].sub, "empty flush took %.3f us",
	      (s.rq[f].done_at - s.rq[f].sub) / 1e3);
	sim_free(&s);

	cur_test = "ssd: without PLP a flush programs the partial page first";
	p = *ssd_profile("sata-consumer");
	ssd_up(&s, &p, 32);
	sim_submit(&s, MODEL_WRITE, 8 * 999, 8, NULL);
	sim_drain(&s, SEC);
	s.now += 10 * MS;			/* idle: the page waits for a flush */
	f = sim_submit(&s, MODEL_FLUSH, 0, 0, NULL);
	s.now += 1000;
	r = sim_submit(&s, MODEL_READ, 8 * 5000, 8, NULL);
	sim_drain(&s, s.now + SEC);
	t = s.rq[f].done_at - s.rq[f].sub;
	CHECK(t + 1000 >= ssd_unit_ns(&p) + p.flush_us * 1e3 &&
	      t <= 2 * ssd_unit_ns(&p) + p.flush_us * 1e3,
	      "flush %.1f us, one page program + flush_us = %.1f us", t / 1e3,
	      (ssd_unit_ns(&p) + p.flush_us * 1e3) / 1e3);
	CHECK(s.rq[r].done_at >= s.rq[f].done_at,
	      "SATA: a read got past a non-queued flush");
	sim_free(&s);

	cur_test = "ssd: an NVMe flush holds nothing up";
	p = *ssd_profile("nvme-plp");
	p.vwc = 1;
	p.plp = 0;
	ssd_up(&s, &p, 128);
	w = sim_submit(&s, MODEL_WRITE, 8 * 999, 8, NULL);
	sim_drain(&s, SEC);
	f = sim_submit(&s, MODEL_FLUSH, 0, 0, NULL);
	s.now += 1000;
	r = sim_submit(&s, MODEL_READ, 8 * 5000, 8, NULL);
	sim_drain(&s, s.now + SEC);
	CHECK(s.rq[r].done_at < s.rq[f].done_at,
	      "NVMe: a read waited for a queued flush");
	(void)w;
	sim_free(&s);

	cur_test = "ssd: vwc 0 means a flush does nothing";
	p = *ssd_profile("nvme-plp");
	ssd_up(&s, &p, 128);
	sim_submit(&s, MODEL_WRITE, 8 * 999, 8, NULL);
	f = sim_submit(&s, MODEL_FLUSH, 0, 0, NULL);
	CHECK(s.rq[f].done_at == s.rq[f].sub, "flush took time with vwc 0");
	sim_drain(&s, SEC);
	sim_free(&s);

	cur_test = "ssd: a full buffer makes writes wait, and they all finish";
	p = *ssd_profile("sata-consumer");
	p.buf_mb = 2;
	ssd_up(&s, &p, 32);
	{
		struct job j = { .op = MODEL_WRITE, .pattern = RAND, .qd = 32, .nr = 8,
				 .lo = 0, .hi = SSD_SECTORS, .stop_at = SEC };

		job_start(&s, &j);
		sim_run_until(&s, SEC);
		sim_drain(&s, 60 * SEC);
		CHECK(s.s->n_buf_full > 0, "the buffer never filled");
		CHECK(j.n > 0, "no writes completed");
	}
	sim_free(&s);
}

/*
 * One die, one page of writes costing one unit: its program starts once
 * the data is on the drive. Returns that start.
 */
static __u64 ssd_one_program(struct sim *s, const struct ssd_params *p)
{
	__u64 start = s->now + p->cmd_us * 1e3 +
		xfer_ns_of(p->iface_mbps, p->page_kb << 10);

	sim_submit(s, MODEL_WRITE, 8 * 999, p->page_kb * 2, NULL);
	return start;
}

/* submit a 4K read at t; its latency */
static double ssd_read_at(struct sim *s, __u64 t, __u64 lba)
{
	int tag;

	sim_run_until(s, t);
	tag = sim_submit(s, MODEL_READ, lba, 8, NULL);
	sim_drain(s, s->now + SEC);
	return s->rq[tag].done_at - s->rq[tag].sub;
}

static void ssd_suspend(void)
{
	struct ssd_params p = *ssd_profile("nvme-plp");
	double lat, want, u, die_read;
	struct sim s;
	__u64 s0, t1;
	int a, b, f;

	p.dies = 1;
	p.waf = 1;
	p.plp = 0;		/* so a flush shows when the die's programs end */
	p.vwc = 1;
	u = ssd_unit_ns(&p);
	die_read = p.tr_us * 1e3 + xfer_ns_of(p.ch_mbps, 4096);

	cur_test = "ssd: susp_us 0, a read on a programming die waits for the program";
	ssd_up(&s, &p, 128);
	s0 = ssd_one_program(&s, &p);
	lat = ssd_read_at(&s, s0 + 100 * US, 8 * 5000);
	want = u - 100 * US + ssd_read_ns(&p);
	CHECK(fabs(lat - want) < 5, "read %.3f us, rest of the program + read = %.3f us",
	      lat / 1e3, want / 1e3);
	sim_free(&s);

	p.susp_us = 20;
	cur_test = "ssd: with susp_us a read on a programming die waits susp_us";
	ssd_up(&s, &p, 128);
	s0 = ssd_one_program(&s, &p);
	t1 = s0 + 100 * US;
	sim_run_until(&s, t1);
	a = sim_submit(&s, MODEL_READ, 8 * 5000, 8, NULL);
	want = p.susp_us * 1e3 + ssd_read_ns(&p);
	CHECK(fabs((double)(s.rq[a].done_at - s.rq[a].sub) - want) < 5,
	      "read %.3f us, susp_us + read = %.3f us",
	      (s.rq[a].done_at - s.rq[a].sub) / 1e3, want / 1e3);

	cur_test = "ssd: a suspended program resumes after the read, later by its die time";
	ssd_one_program(&s, &p);
	f = sim_submit(&s, MODEL_FLUSH, 0, 0, NULL);
	sim_drain(&s, s.now + SEC);
	want = s0 + 2 * u + die_read + p.flush_us * 1e3;
	CHECK(fabs((double)s.rq[f].done_at - want) < 5,
	      "flush after the second page at %.3f us, two programs + read = %.3f us",
	      (s.rq[f].done_at - s0) / 1e3, (want - s0) / 1e3);
	sim_free(&s);

	cur_test = "ssd: without PLP a flush waits for the suspended program to finish";
	ssd_up(&s, &p, 128);
	s0 = ssd_one_program(&s, &p);
	sim_run_until(&s, s0 + 100 * US);
	sim_submit(&s, MODEL_READ, 8 * 5000, 8, NULL);
	f = sim_submit(&s, MODEL_FLUSH, 0, 0, NULL);
	sim_drain(&s, s.now + SEC);
	want = s0 + u + die_read + p.flush_us * 1e3;
	CHECK(fabs((double)s.rq[f].done_at - want) < 5,
	      "flush at %.3f us, program + read = %.3f us",
	      (s.rq[f].done_at - s0) / 1e3, (want - s0) / 1e3);
	sim_free(&s);

	cur_test = "ssd: a read with less than susp_us of program left waits for its end";
	ssd_up(&s, &p, 128);
	s0 = ssd_one_program(&s, &p);
	lat = ssd_read_at(&s, s0 + (__u64)u - 10 * US, 8 * 5000);
	want = 10 * US + ssd_read_ns(&p);
	CHECK(fabs(lat - want) < 5, "read %.3f us, 10 us of program + read = %.3f us",
	      lat / 1e3, want / 1e3);
	sim_free(&s);

	cur_test = "ssd: a read arriving during a suspension queues without another susp_us";
	ssd_up(&s, &p, 128);
	s0 = ssd_one_program(&s, &p);
	t1 = s0 + 100 * US;
	sim_run_until(&s, t1);
	sim_submit(&s, MODEL_READ, 8 * 5000, 8, NULL);
	sim_run_until(&s, t1 + US);
	b = sim_submit(&s, MODEL_READ, 8 * 6000, 8, NULL);
	want = p.susp_us * 1e3 + die_read + ssd_read_ns(&p) - US;
	CHECK(fabs((double)(s.rq[b].done_at - s.rq[b].sub) - want) < 5,
	      "second read %.3f us, susp_us + first read's die time + read - 1 us "
	      "= %.3f us", (s.rq[b].done_at - s.rq[b].sub) / 1e3, want / 1e3);
	sim_drain(&s, s.now + SEC);
	sim_free(&s);

	cur_test = "ssd: a read while the page still goes to the die waits for the whole program";
	p.ch_mbps = 100;	/* 16K takes 164 us to reach the die */
	u = ssd_unit_ns(&p);
	ssd_up(&s, &p, 128);
	s0 = ssd_one_program(&s, &p);
	lat = ssd_read_at(&s, s0 + 50 * US, 8 * 5000);
	want = u - 50 * US + ssd_read_ns(&p);
	CHECK(fabs(lat - want) < 5, "read %.3f us, rest of the program + read = %.3f us",
	      lat / 1e3, want / 1e3);
	cur_test = "ssd: once the page is on the die a read waits susp_us";
	s0 = ssd_one_program(&s, &p);
	lat = ssd_read_at(&s, s0 + xfer_ns_of(p.ch_mbps, p.page_kb << 10) + 10 * US,
			  8 * 5000);
	want = p.susp_us * 1e3 + ssd_read_ns(&p);
	CHECK(fabs(lat - want) < 5, "read %.3f us, susp_us + read = %.3f us",
	      lat / 1e3, want / 1e3);
	sim_free(&s);

	cur_test = "ssd: a half unit receives half a page before it can be suspended";
	p.waf = 1.5;		/* one random page: a whole unit, then half a unit */
	for (int late = 0; late < 2; late++) {
		double half_in = xfer_ns_of(p.ch_mbps, p.page_kb << 10) / 2;
		__u64 s1;

		ssd_up(&s, &p, 128);
		s1 = ssd_one_program(&s, &p) + (__u64)u;	/* second unit */
		lat = ssd_read_at(&s, s1 + (late ? 1.5 : 0.5) * half_in, 8 * 5000);
		want = late ? p.susp_us * 1e3 + ssd_read_ns(&p) :
			u / 2 - 0.5 * half_in + ssd_read_ns(&p);
		CHECK(fabs(lat - want) < 5, "read %s the half page's transfer: %.3f us, "
		      "expected %.3f us", late ? "after" : "during", lat / 1e3, want / 1e3);
		sim_free(&s);
	}
}

/* 4K read latency above the documented tr_us sum, ns */
static double ssd_read_extra(struct sim *s, const struct ssd_params *p,
			     __u64 lba, __u64 nr)
{
	double base = (p->tr_us + p->cmd_us + p->iface_us) * 1e3 +
		xfer_ns_of(p->ch_mbps, nr << 9) + xfer_ns_of(p->iface_mbps, nr << 9);
	int tag = sim_submit(s, MODEL_READ, lba, nr, NULL);
	double lat = s->rq[tag].done_at - s->rq[tag].sub;

	sim_drain(s, s->now + SEC);
	s->now += MS;
	return lat - base;
}

static void ssd_read_levels(void)
{
	struct ssd_params p = *ssd_profile("nvme-plp");
	double step, e, share[3] = { 0 };
	struct sim s;
	int i, k, bad = 0, moved = 0, n = 3000;
	__u64 lba[100];
	double first[100];

	p.tr_step_us = 30;
	step = p.tr_step_us * 1e3;
	cur_test = "ssd: TLC 4K reads take tr_us + 0, 1 or 3 steps, a third each";
	ssd_up(&s, &p, 128);
	for (i = 0; i < n; i++) {
		__u64 a = 8 * rnd_below(SSD_SECTORS / 8);

		e = ssd_read_extra(&s, &p, a, 8);
		for (k = 0; k < 3; k++)
			if (fabs(e - (k == 2 ? 3 : k) * step) < 5)
				break;
		if (k == 3)
			bad++;
		else
			share[k] += 1.0 / n;
		if (i < 100) {
			lba[i] = a;
			first[i] = e;
		}
	}
	CHECK(!bad, "%d of %d reads not tr_us + 0, 1 or 3 steps", bad, n);
	for (k = 0; k < 3; k++)
		CHECK(fabs(share[k] - 1.0 / 3) < 0.04, "page type %d: %.3f of the reads",
		      k, share[k]);

	cur_test = "ssd: a 4K read takes the same time every time";
	for (i = 0; i < 100; i++)
		if (fabs(ssd_read_extra(&s, &p, lba[i], 8) - first[i]) > 5)
			moved++;
	CHECK(!moved, "%d of 100 addresses read at another speed the second time",
	      moved);

	cur_test = "ssd: a 16K read takes its slowest 4K";
	bad = 0;
	for (i = 0; i < 200; i++) {
		__u64 a = 32 * rnd_below(SSD_SECTORS / 32);
		double most = 0, all;

		for (k = 0; k < 4; k++) {
			e = ssd_read_extra(&s, &p, a + 8 * k, 8);
			if (e > most)
				most = e;
		}
		all = ssd_read_extra(&s, &p, a, 32);
		if (fabs(all - most) > 5)
			bad++;
	}
	CHECK(!bad, "%d of 200 16K reads not as slow as their slowest 4K", bad);
	sim_free(&s);
}

/* latency of a read of [lba, lba + nr) once earlier programs are done */
static double ssd_quiet_read(struct sim *s, __u64 lba, __u64 nr)
{
	int tag;

	sim_run_until(s, s->now + 100 * MS);
	tag = sim_submit(s, MODEL_READ, lba, nr, NULL);
	sim_drain(s, s->now + SEC);
	return s->rq[tag].done_at - s->rq[tag].sub;
}

static void ssd_write_now(struct sim *s, __u64 lba, __u64 nr)
{
	sim_submit(s, MODEL_WRITE, lba, nr, NULL);
	sim_drain(s, s->now + SEC);
}

static void ssd_history(void)
{
	struct ssd_params p = *ssd_profile("nvme-plp");
	__u64 c = 128 * 20, i;		/* 64 KiB chunk number 20 */
	double whole, frags, lat;
	struct sim s;

	/* one die and no read levels: every cost is a plain sum */
	p.dies = 1;
	p.page_kb = 64;
	p.tr_step_us = 0;
	whole = (p.tr_us + p.cmd_us + p.iface_us) * 1e3 +
		xfer_ns_of(p.ch_mbps, 65536) + xfer_ns_of(p.iface_mbps, 65536);
	frags = 16 * (p.tr_us * 1e3 + xfer_ns_of(p.ch_mbps, 4096)) +
		(p.cmd_us + p.iface_us) * 1e3 + xfer_ns_of(p.iface_mbps, 65536);

	cur_test = "ssd: a chunk written in one piece reads as one page";
	ssd_up(&s, &p, 32);
	lat = ssd_quiet_read(&s, c, 128);
	CHECK(fabs(lat - whole) < 5, "64K read %.3f us, one page read %.3f us",
	      lat / 1e3, whole / 1e3);

	cur_test = "ssd: a 4K random write scatters its chunk: 16 page reads";
	ssd_write_now(&s, c + 40, 8);
	lat = ssd_quiet_read(&s, c, 128);
	CHECK(fabs(lat - frags) < 20, "64K read %.3f us, 16 x 4K page reads %.3f us",
	      lat / 1e3, frags / 1e3);

	cur_test = "ssd: a write covering the chunk puts it in one piece";
	ssd_write_now(&s, c, 128);
	lat = ssd_quiet_read(&s, c, 128);
	CHECK(fabs(lat - whole) < 5, "64K read %.3f us, one page read %.3f us",
	      lat / 1e3, whole / 1e3);

	cur_test = "ssd: a sequential stream puts a chunk in one piece when it completes it";
	ssd_write_now(&s, c + 120, 8);	/* scatter it again */
	for (i = 0; i < 16; i++) {
		ssd_write_now(&s, c + 8 * i, 8);
		if (i == 7) {
			lat = ssd_quiet_read(&s, c, 128);
			CHECK(fabs(lat - frags) < 20, "half written by the stream: "
			      "%.3f us, still 16 page reads %.3f us", lat / 1e3,
			      frags / 1e3);
		}
	}
	lat = ssd_quiet_read(&s, c, 128);
	CHECK(fabs(lat - whole) < 5, "completed by the stream: %.3f us, one "
	      "page read %.3f us", lat / 1e3, whole / 1e3);
	sim_free(&s);

	cur_test = "ssd: history rnd starts with every chunk scattered";
	p.history = 1;
	ssd_up(&s, &p, 32);
	lat = ssd_quiet_read(&s, c, 128);
	CHECK(fabs(lat - frags) < 20, "64K read %.3f us, 16 x 4K page reads %.3f us",
	      lat / 1e3, frags / 1e3);
	sim_free(&s);
}

/* program units for n 4K random writes at QD32 after `idle` ns of nothing */
static double ssd_units_after_idle(const struct ssd_params *p, __u64 idle,
				   __u64 n)
{
	struct sim s;
	double u;
	__u64 i;

	ssd_up(&s, p, 32);
	s.deep_check = 1;
	sim_run_until(&s, idle);
	for (i = 0; i < n; i++) {
		while (s.inflight == 32 && sim_step(&s, NONE))
			;
		sim_submit(&s, MODEL_WRITE, 8 * rnd_below(SSD_SECTORS / 8), 8, NULL);
	}
	sim_drain(&s, s.now + 60 * SEC);
	sim_run_until(&s, s.now + 60 * SEC);
	u = s.s->units_done;
	sim_free(&s);
	return u;
}

static void ssd_gc_pool(void)
{
	struct ssd_params p = *ssd_profile("nvme-plp");
	double page = p.page_kb << 10, n = 16384, bytes = n * 4096, pool, want, u;

	/*
	 * Writes arrive faster than the dies program: pages wait from the
	 * first one on, so the pool fills only during the idle second.
	 */
	p.gc_pool_mb = 1024;
	p.gc_mbps = 32;
	pool = p.gc_mbps * 1e6;
	cur_test = "ssd: after idle, random writes use the erased space GC prepared";
	u = ssd_units_after_idle(&p, SEC, n);
	want = (pool + (bytes - pool) * p.waf) / page;
	CHECK(fabs(u - want) / want < 0.03, "%.0f program units, expected (pool "
	      "+ rest x waf) / page = %.0f", u, want);

	cur_test = "ssd: the pool stops at gc_pool_mb";
	p.gc_pool_mb = 8;
	pool = 8 * 1048576.0;
	u = ssd_units_after_idle(&p, SEC, n);
	want = (pool + (bytes - pool) * p.waf) / page;
	CHECK(fabs(u - want) / want < 0.03, "%.0f program units, expected %.0f",
	      u, want);

	cur_test = "ssd: the pool fills only once the drive has been idle gc_idle_s";
	p.gc_pool_mb = 1024;
	p.gc_idle_s = 0.5;
	pool = p.gc_mbps * 1e6 * 0.5;
	u = ssd_units_after_idle(&p, SEC, n);
	want = (pool + (bytes - pool) * p.waf) / page;
	CHECK(fabs(u - want) / want < 0.03, "%.0f program units, expected %.0f",
	      u, want);
	p.gc_idle_s = 0;

	cur_test = "ssd: without gc_mbps every page of random writes costs waf";
	p.gc_pool_mb = 1024;
	p.gc_mbps = 0;
	u = ssd_units_after_idle(&p, SEC, n);
	want = bytes * p.waf / page;
	CHECK(fabs(u - want) / want < 0.03, "%.0f program units, expected %.0f",
	      u, want);
}

/* ---- randomized invariants --------------------------------------- */

static void random_workload(struct sim *s, __u64 max_nr, int flushes,
			    __u64 n)
{
	__u64 i, gap_mean = 1 + rnd_below(3 * MS), last_end = 0;

	for (i = 0; i < n; i++) {
		int op, kind = rnd_below(100);
		__u64 nr = 1 + rnd_below(max_nr), lba, span;

		s->now += rnd_below(2 * gap_mean);
		while (s->inflight == (int)s->depth && sim_step(s, NONE))
			;
		sim_run_until(s, s->now);
		if (flushes && kind < 5)
			op = MODEL_FLUSH;
		else
			op = kind < 55 ? MODEL_READ : MODEL_WRITE;
		span = (s->h ? HDD_SECTORS : SSD_SECTORS) - nr;
		lba = (op == MODEL_WRITE && rnd_below(3) == 0) ? last_end :
			rnd_below(span);
		if (lba > span)
			lba = 0;
		if (op == MODEL_WRITE)
			last_end = lba + nr;
		sim_submit(s, op, op == MODEL_FLUSH ? 0 : lba,
			   op == MODEL_FLUSH ? 0 : nr, NULL);
	}
	sim_drain(s, s->now + 600 * SEC);
}

static void hdd_random(int runs)
{
	static const unsigned cache[] = { 0, 1, 4, 64 }, ncq[] = { 1, 4, 32 },
		rpm[] = { 5400, 7200, 15000 }, window[] = { 0, 1, 4 };
	static const double wait[] = { 0, 50, 500 };
	int i;

	for (i = 0; i < runs; i++) {
		struct hdd_params p = hdd_p();
		int flushes = rnd_below(2);
		struct sim s;
		double full, op, wb;

		p.cache_mb = cache[rnd_below(4)];
		p.ncq = ncq[rnd_below(3)];
		p.rpm = rpm[rnd_below(3)];
		p.max_wait_ms = wait[rnd_below(3)];
		p.wb_window = window[rnd_below(3)];
		p.seed = rnd();
		hdd_up(&s, &p, 32);
		s.deep_check = 1;
		full = s.h->seek_full_ms * 1e6;
		op = full + rev_ns(&p) + xfer_ns_of(p.mbps, 1 << 20);
		wb = full + 2 * rev_ns(&p);	/* a forced write-back: one track */
		/*
		 * Once past the limit a read waits at most for the requests
		 * past it before it, each after one forced write-back.
		 */
		if (p.max_wait_ms && !flushes)
			s.read_bound = p.max_wait_ms * 1e6 + 32 * (op + wb) + op;
		cur_test = "hdd randomized: invariants";
		random_workload(&s, 2048, flushes, 1500);
		sim_free(&s);
	}
}

static void ssd_random(int runs)
{
	static const unsigned dies[] = { 1, 4, 32 }, page[] = { 4, 16 },
		buf[] = { 2, 8, 32 };
	int i;

	for (i = 0; i < runs; i++) {
		struct ssd_params p = *ssd_profile("sata-plp");
		unsigned depth;
		struct sim s;

		p.nvme = rnd_below(2);
		p.plp = rnd_below(2);
		p.vwc = rnd_below(2);
		p.dies = dies[rnd_below(3)];
		p.page_kb = page[rnd_below(2)];
		p.buf_mb = buf[rnd_below(3)];
		p.waf = 1 + rnd_below(5);
		p.floor_us = rnd_below(2) ? 0 : 20;
		p.susp_us = rnd_below(2) ? 0 : 20;
		p.tr_step_us = rnd_below(2) ? 0 : 26;
		p.history = rnd_below(2);
		p.gc_pool_mb = rnd_below(2) ? 0 : 4;
		p.gc_mbps = rnd_below(2) ? 0 : 50;
		p.gc_idle_s = rnd_below(2) ? 0 : 0.002;
		p.cmd_us = p.nvme ? (rnd_below(2) ? 0 : 0.56) : 3;
		depth = p.nvme ? 64 : 32;
		ssd_up(&s, &p, depth);
		s.deep_check = 1;
		cur_test = "ssd randomized: invariants";
		random_workload(&s, 2048, 1, 3000);
		sim_free(&s);
	}
}

int main(int argc, char **argv)
{
	int runs = argc > 1 ? atoi(argv[1]) : 100;

	hdd_seq_read_is_transfer();
	hdd_random_read_latency();
	hdd_cached_write_and_flush();
	hdd_writeback_by_track();
	hdd_wb_window();
	hdd_writer_next_to_readers();
	hdd_age_limit();
	ssd_qd1_costs();
	ssd_die_parallelism();
	ssd_rates();
	ssd_seq_over_scattered();
	ssd_flushes();
	ssd_suspend();
	ssd_read_levels();
	ssd_history();
	ssd_gc_pool();
	hdd_random(runs);
	ssd_random(runs);

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
