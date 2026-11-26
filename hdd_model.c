// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: 2026 Lumir Sliva
/*
 * hdd: timing model of a single-actuator hard disk, on top of a backing
 * device (normally RAM).
 *
 * Data goes to the backing device immediately; the model only decides when
 * each request completes. What it models:
 *
 *  - one actuator: reads and write-through writes are served one at a time,
 *    shortest-positioning-time-first among the oldest `ncq` waiting
 *    requests (NCQ with rotational position ordering), except that one
 *    passed over for max_wait_ms is served next;
 *  - positioning = seek(distance, none within a track) + wait for the
 *    sector to rotate under the head (sector angle = lba / sectors per
 *    track, platter phase = time / revolution), then media transfer; a
 *    read, a write-back or an already queued write that starts where the
 *    head is costs no positioning (see pos_ns);
 *  - a volatile write cache (cache_mb > 0): writes complete after the host
 *    transfer and are written back by the actuator
 *    shortest-positioning-time-first over the whole cache, or over the
 *    wb_window oldest dirty extents if set, at most one track per
 *    operation so queued reads get a turn in between. Write-back
 *    runs when the actuator has nothing else to do; once the cache is 3/4
 *    full or writes wait for space, it also alternates with the queue,
 *    one write-back per queued request served (one that started while the
 *    queue was empty counts);
 *  - FLUSH as SATA FLUSH CACHE: a non-queued command. Requests queued before
 *    it are served, the whole dirty cache is destaged, and everything that
 *    arrives meanwhile (reads included) waits until the flush completes;
 *  - optionally, a drive that logs instead (log_ms / log_us > 0): a flush
 *    appends the extents no flush has logged yet to a log on the platters
 *    (log_ms once, log_us and the media transfer per extent) and
 *    completes. The extents stay in the cache, which is then the set of
 *    writes not yet in place, bounded by cache_mb and by cache_ext
 *    extents. With wb_idle_ms the actuator writes back only once no
 *    command has arrived for that long, or when the cache has no room
 *    for another write: a burst is taken at interface speed until the
 *    cache is full, then at the rate of write-back; a write that
 *    continues the previous one and finds no room goes straight to the
 *    platters. wsettle_ms is added to every seek that ends in a write;
 *  - wb_turns, wb_queue: how a cache with no room shares the actuator
 *    with the queue: wb_turns write-backs per queued request served, at
 *    most wb_queue / (requests queued);
 *  - capacity_gb: the device stands for a drive of that size, so two
 *    requests share a track (no seek) only as often as on that drive;
 *  - write-through (cache_mb = 0): no volatile cache is advertised, so the
 *    kernel never sends flushes and every write pays mechanical time.
 *
 *  - optionally zones (mbps_inner): rate and track length fall with the
 *    radius from mbps at the start of the drive to mbps_inner at its end;
 *
 * Not modelled: read
 * cache / read-ahead beyond "sequential costs no positioning", thermal
 * recalibration, a fixed cost per flush.
 * Calibrate against the drive you want to imitate before trusting
 * absolute numbers.
 *
 * No clock and no I/O of its own: time, completions and wake-ups go
 * through struct model_env (model.h). The kublk target is hdd.c.
 */

#include "hdd_model.h"
#if defined(MODEL_CHECK_SPTF) && defined(NDEBUG)
#error MODEL_CHECK_SPTF needs assert(): build without NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* platter phase in [0, 1) at time t */
static double phase_at(struct hdd_model *m, double t)
{
	double x = t / m->period_ns + m->phase0;

	return x - floor(x);
}

/* zoned: rate and track length at `lba`, as a fraction of the outermost */
static double zone_f(const struct hdd_model *m, __u64 lba)
{
	double x = 1 - m->za * (double)lba;

	return sqrt(x > 0 ? x : 0);
}

/*
 * Tracks from the start of the device to `lba`: the integral of
 * 1 / (track length). Its fraction is the sector's angle.
 */
static double tracks_to(const struct hdd_model *m, double lba)
{
	double x;

	if (!(m->za > 0))
		return lba / m->spt;
	/* (1 - sqrt(x)) / za, without the cancellation when za * lba is small */
	x = 1 - m->za * lba;
	return 2 * lba / (m->spt * (1 + sqrt(x > 0 ? x : 0)));
}

/*
 * Media transfer of nr sectors at lba. Zoned: the time the platter takes
 * to turn through them, so that a transfer ends where the angle of its
 * last sector says.
 */
static __u64 media_ns(const struct hdd_model *m, __u64 lba, __u64 nr)
{
	if (!(m->za > 0))
		return (__u64)((double)(nr << 9) / (m->p.mbps * 1e6) * 1e9);
	return (__u64)((tracks_to(m, (double)(lba + nr)) -
			tracks_to(m, (double)lba)) * m->period_ns);
}

/* seek time for a distance of `d` sectors to another track */
static double seek_ns(struct hdd_model *m, double d)
{
	d = d / (double)m->dev_sectors * m->p.stroke;
	if (d > 1)
		d = 1;
	return (m->p.seek_min_ms + (m->seek_full_ms - m->p.seek_min_ms) *
		sqrt(d)) * 1e6;
}

/*
 * Time from `t` until the head can start transferring `to`, coming from
 * the end of the previous transfer at `from`. If `contig` and the request
 * continues where the head is, it costs nothing: reads (read-ahead),
 * destage (the drive's own queue) and writes that were already queued
 * when the previous transfer ended. Otherwise: seek (none within the same
 * track), then wait for the sector to come round; for a write-through
 * write arriving after the previous one completed, that is most of a
 * revolution, as on a real drive. Sector angle is the fraction of
 * tracks_to(lba) (lba / spt without zones), so a
 * transfer ending at lba e ends at phase e / spt, and choosing the minimum
 * of this over queued requests is rotational position ordering.
 */
static double pos_ns(struct hdd_model *m, double t, __u64 from, __u64 to,
		     int contig, int wr)
{
	double seek = 0, ang, rot;
	int diff;

	if (from == to && contig)
		return 0;
	if (m->za > 0) {
		double k = m->spt / m->tspt;

		ang = tracks_to(m, (double)to);
		diff = (__u64)(tracks_to(m, (double)from) * k) != (__u64)(ang * k);
	} else {
		diff = (__u64)(from / m->tspt) != (__u64)(to / m->tspt);
		ang = (double)to / m->spt;
	}
	if (diff) {
		seek = seek_ns(m, fabs((double)to - (double)from));
		if (wr)
			seek += m->p.wsettle_ms * 1e6;
	}
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

/* index of the first dirty extent ending at or after `lba` */
static int dirty_find_end(struct hdd_model *m, __u64 lba)
{
	int lo = 0, hi = m->ndirty;

	while (lo < hi) {
		int mid = (lo + hi) / 2;

		if (m->dirty[mid].lba + m->dirty[mid].nr < lba)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/*
 * Add [lba, lba+nr) to the dirty set, merging overlapping/adjacent
 * extents. The set stays sorted by LBA, disjoint and non-adjacent.
 */
static void dirty_add(struct hdd_model *m, __u64 lba, __u64 nr, __u64 t)
{
	__u64 s = lba, e = lba + nr, seq = m->next_seq++, unl = nr;
	int i = dirty_find_end(m, s), j = i;

	while (j < m->ndirty && m->dirty[j].lba <= e) {
		struct hdd_ext *x = &m->dirty[j++];

		if (x->lba < s)
			s = x->lba;
		if (x->lba + x->nr > e)
			e = x->lba + x->nr;
		if (x->seq < seq)
			seq = x->seq;
		m->dirty_bytes -= x->nr << 9;
		/* what a flush logged of the pieces stays logged */
		if (x->unl) {
			unl += x->unl;
			m->unlogged--;
			m->unlogged_bytes -= x->unl << 9;
		}
	}
	if (j == i) {		/* nothing merged: make room at i */
		if (m->ndirty == m->cap_dirty) {
			m->cap_dirty *= 2;
			m->dirty = realloc(m->dirty,
					   m->cap_dirty * sizeof(*m->dirty));
			assert(m->dirty);
		}
		memmove(m->dirty + i + 1, m->dirty + i,
			(m->ndirty - i) * sizeof(*m->dirty));
		m->ndirty++;
	} else if (j > i + 1) {	/* several merged into slot i */
		memmove(m->dirty + i + 1, m->dirty + j,
			(m->ndirty - j) * sizeof(*m->dirty));
		m->ndirty -= j - i - 1;
	}
	m->dirty[i].lba = s;
	m->dirty[i].nr = e - s;
	m->dirty[i].added = t;
	m->dirty[i].seq = seq;
	/*
	 * A write over unlogged data is counted twice up to here; the cap
	 * only keeps it within the extent, so rewriting a block of a large
	 * dirty extent many times makes the next flush dearer than it is.
	 */
	if (unl > e - s)
		unl = e - s;
	m->dirty[i].unl = unl;
	m->dirty_bytes += (e - s) << 9;
	m->unlogged++;
	m->unlogged_bytes += unl << 9;
}

static void arrive(struct hdd_model *m, struct hdd_req r, __u64 now);

static __u64 max_u64(__u64 a, __u64 b)
{
	return a > b ? a : b;
}

/* room in the cache for a write of nr sectors */
static int cache_room(const struct hdd_model *m, __u64 nr)
{
	if (m->dirty_bytes + (nr << 9) > m->cache_bytes)
		return 0;
	return !m->p.cache_ext ||
		m->ndirty + m->destage_ext < (int)m->p.cache_ext;
}

/* after a write of nr sectors entered the cache at t: is it full now? */
static void cache_filled(struct hdd_model *m, __u64 nr, __u64 t)
{
	m->last_nr = nr;
	if (!cache_room(m, nr))
		m->full_at = t;
}

#define HDD_WB_WINDOW_MAX 64

/*
 * wb_window N: the drive only reorders its write-back among the N extents
 * that entered the cache first (a small firmware queue), shortest
 * positioning time among those. Found by one pass keeping the N oldest
 * (by arrival order, `seq`: several writes can arrive at the same time)
 * in a small sorted array.
 */
static int best_dirty_window(struct hdd_model *m, __u64 free_at,
			     double *bready)
{
	int idx[HDD_WB_WINDOW_MAX], n = 0, w = (int)m->p.wb_window, i, j;
	int best = -1;

	assert(w > 0 && w <= HDD_WB_WINDOW_MAX);
	for (i = 0; i < m->ndirty; i++) {
		__u64 a = m->dirty[i].seq;

		if (n == w && a >= m->dirty[idx[n - 1]].seq)
			continue;
		j = n < w ? n++ : n - 1;
		while (j > 0 && m->dirty[idx[j - 1]].seq > a) {
			idx[j] = idx[j - 1];
			j--;
		}
		idx[j] = i;
	}
	for (j = 0; j < n; j++) {
		const struct hdd_ext *x = &m->dirty[idx[j]];
		__u64 t = max_u64(free_at, x->added);
		double ready = t + pos_ns(m, t, m->head, x->lba, 1, 1);

		if (best < 0 || ready < *bready) {
			*bready = ready;
			best = idx[j];
		}
	}
	return best;
}

/*
 * The dirty extent whose write-back can start first, and when:
 * shortest positioning time over the whole cache. The set is sorted by
 * LBA, so search outward from the head and stop in each direction once
 * the seek alone to the next extent can't beat the best found; the seek
 * grows with distance, and within a track it can be 0.
 */
static int best_dirty(struct hdd_model *m, __u64 free_at, double *bready)
{
	int k, i, best = -1;

	if (m->p.wb_window && m->ndirty > (int)m->p.wb_window)
		return best_dirty_window(m, free_at, bready);
	k = dirty_find_end(m, m->head);

	for (i = k; i < m->ndirty; i++) {
		const struct hdd_ext *x = &m->dirty[i];
		double d = x->lba > m->head ? (double)(x->lba - m->head) : 0;
		__u64 t = max_u64(free_at, x->added);
		double ready;

		if (best >= 0 && d >= m->tspt &&
		    free_at + seek_ns(m, d) >= *bready)
			break;
		ready = t + pos_ns(m, t, m->head, x->lba, 1, 1);
		if (best < 0 || ready < *bready) {
			*bready = ready;
			best = i;
		}
	}
	for (i = k - 1; i >= 0; i--) {
		const struct hdd_ext *x = &m->dirty[i];
		double d = (double)(m->head - x->lba);
		__u64 t = max_u64(free_at, x->added);
		double ready;

		if (best >= 0 && d >= m->tspt &&
		    free_at + seek_ns(m, d) >= *bready)
			break;
		ready = t + pos_ns(m, t, m->head, x->lba, 1, 1);
		if (best < 0 || ready < *bready) {
			*bready = ready;
			best = i;
		}
	}
#ifdef MODEL_CHECK_SPTF
	/* tests: the pruned search finds what a full scan finds */
	for (i = 0; i < m->ndirty; i++) {
		__u64 t = max_u64(free_at, m->dirty[i].added);

		assert(t + pos_ns(m, t, m->head, m->dirty[i].lba, 1, 1) >= *bready);
	}
#endif
	return best;
}

/*
 * Write back at most one track of dirty extent i, starting at `ready`. The
 * rest stays dirty and costs no positioning if it is taken next; the cache
 * space is freed when the transfer ends (pump).
 */
static void destage(struct hdd_model *m, int i, double ready)
{
	struct hdd_ext *x = &m->dirty[i];
	__u64 nr = x->nr, track = (__u64)(m->za > 0 ?
		m->spt * zone_f(m, x->lba) : m->spt);

	if (m->za > 0 && !track)
		track = 1;

	if (track && nr > track)
		nr = track;
	m->busy_until = (__u64)ready + media_ns(m, x->lba, nr);
	m->head = x->lba + nr;
	m->destaging = nr << 9;
	m->destage_ext = nr == x->nr;
	if (x->unl) {
		/*
		 * Which sectors of an extent are the unlogged ones is not
		 * kept: take them to be its end, so this piece carries only
		 * what the rest cannot hold. They count as unlogged until
		 * the transfer ends (pump).
		 */
		__u64 u = x->unl > x->nr - nr ? x->unl - (x->nr - nr) : 0;

		x->unl -= u;
		m->destage_unl = u;
		m->destage_unl_ext = u && !x->unl;
	}
	if (nr == x->nr) {
		memmove(m->dirty + i, m->dirty + i + 1,
			(m->ndirty - i - 1) * sizeof(*m->dirty));
		m->ndirty--;
	} else {
		x->lba += nr;
		x->nr -= nr;
	}
	m->n_destage++;
	if (m->wb_turn < (1 << 30))	/* only "at least wb_turns" matters */
		m->wb_turn++;
}

/*
 * With wb_idle_ms the actuator does not write back on its own while
 * commands keep coming, so a write-back cannot start before what
 * triggered it: the cache filling up, a write waiting for room, a flush
 * that waits for the cache to drain, or wb_idle_ms after the latest
 * command.
 */
static __u64 wb_since(const struct hdd_model *m)
{
	if (!cache_room(m, m->last_nr))
		return m->full_at;
	if (m->nwait)
		return m->wait[0].arrive;
	if (m->flush_tag >= 0 && !m->log)
		return m->flush_start;
	return m->last_cmd + m->wb_idle_ns;
}

/* write-backs a full cache gets before the next queued request is served */
static int wb_turns(const struct hdd_model *m)
{
	int n = (int)m->p.wb_turns;

	if (m->p.wb_queue && n > (int)m->p.wb_queue / m->npend)
		n = (int)m->p.wb_queue / m->npend;
	return n > 1 ? n : 1;
}

/*
 * Run the actuator up to `now`. The actuator keeps its own timeline:
 * each operation starts when the actuator became free or when its request
 * arrived, whichever is later, not when this function happens to run, so
 * the model doesn't depend on how promptly timers fire. Completions that
 * fall in the (recent) past fire at once.
 */
static __u64 pump(struct hdd_model *m)
{
	__u64 now = m->env.now(m->env.ctx);

	for (;;) {
		__u64 free_at = m->busy_until;

		if (free_at > now) {
			m->env.wake(m->env.ctx, free_at);
			break;
		}

		/* the last write-back is on the platters: its space is free */
		m->dirty_bytes -= m->destaging;
		m->destaging = 0;
		m->destage_ext = 0;
		m->unlogged_bytes -= m->destage_unl << 9;
		m->unlogged -= m->destage_unl_ext;
		m->destage_unl = 0;
		m->destage_unl_ext = 0;

		/* cache space freed by destaging admits waiting writes */
		while (m->nwait && cache_room(m, m->wait[0].nr)) {
			struct hdd_req w = m->wait[0];
			__u64 t = max_u64(free_at, w.arrive);

			memmove(m->wait, m->wait + 1, --m->nwait * sizeof(*m->wait));
			dirty_add(m, w.lba, w.nr, t);
			cache_filled(m, w.nr, t);
			m->env.done(m->env.ctx, w.tag,
				    t + host_ns(m, w.nr));
		}

		if (m->npend) {
			int n = m->npend < (int)m->p.ncq ? m->npend : (int)m->p.ncq;
			int i, best = -1;
			double ready, bready = 0;
			struct hdd_req r;

			/*
			 * A cache 3/4 full (or writes waiting for space) forces
			 * write-back instead of waiting for an idle actuator,
			 * as a drive does before its cache runs out: one
			 * write-back per queued request served, or wb_turns()
			 * of them, counting those that started while the
			 * queue was empty. (Comparing
			 * ready times would starve the queue: the best of
			 * thousands of dirty extents is nearly always closer
			 * than the best of a few queued reads.)
			 */
			if (m->ndirty && m->wb_turn < wb_turns(m) &&
			    (m->nwait ||
			    (m->wb_idle_ns ? !cache_room(m, m->last_nr) :
			     m->dirty_bytes >= m->cache_bytes / 4 * 3))) {
				double dready = 0;
				/*
				 * free_at is never before the trigger here:
				 * a cache that filled with an idle actuator
				 * started its write-back below at once.
				 */
				int d = best_dirty(m, free_at, &dready);

				destage(m, d, dready);
				continue;
			}

			/*
			 * Earliest start of transfer first (NCQ with RPO), but
			 * a request passed over for max_wait_ms goes next: the
			 * queue is in arrival order, pend[0] is the oldest.
			 */
			if (m->max_wait_ns && free_at >= m->pend[0].arrive +
			    m->max_wait_ns)
				n = 1;
			for (i = 0; i < n; i++) {
				const struct hdd_req *c = &m->pend[i];
				__u64 t = max_u64(free_at, c->arrive);
				int contig = c->op == MODEL_READ ||
					c->arrive <= free_at;

				ready = t + pos_ns(m, t, m->head, c->lba, contig,
						   c->op == MODEL_WRITE);
				if (best < 0 || ready < bready) {
					bready = ready;
					best = i;
				}
			}
			m->wb_turn = 0;

			r = m->pend[best];
			memmove(m->pend + best, m->pend + best + 1,
				(--m->npend - best) * sizeof(*m->pend));
			m->busy_until = (__u64)bready +
				media_ns(m, r.lba, r.nr);
			m->head = r.lba + r.nr;
			m->env.done(m->env.ctx, r.tag, m->busy_until);
			continue;
		}

		if (m->flush_tag >= 0 && !m->nwait && (m->log || !m->ndirty)) {
			__u64 t = max_u64(free_at, m->flush_start);
			__u64 d;
			struct hdd_req *held = m->blocked;
			int i, n = m->nblocked;

			/*
			 * Log what no flush has logged yet: the actuator is
			 * busy for it, the head comes back where it was.
			 */
			if (m->log && !m->logging && m->unlogged) {
				m->busy_until = t + (__u64)(m->p.log_ms * 1e6 +
					m->unlogged * m->p.log_us * 1e3) +
					xfer_ns(m->p.mbps,
						m->unlogged_bytes >> 9);
				for (i = 0; i < m->ndirty; i++)
					m->dirty[i].unl = 0;
				m->unlogged = 0;
				m->unlogged_bytes = 0;
				m->logging = 1;
				m->n_logged++;
				continue;
			}
			m->logging = 0;
			m->last_cmd = t;	/* idle time counts from here */
			d = t - m->flush_start;

			m->env.done(m->env.ctx, m->flush_tag, t);
			m->n_flush++;
			m->flush_ns_sum += d;
			if (d > m->flush_ns_max)
				m->flush_ns_max = d;
			m->flush_tag = -1;

			/*
			 * Replay what the flush held back, in arrival order;
			 * anything a replayed flush holds again goes to the
			 * other array.
			 */
			m->blocked = m->spare;
			m->spare = held;
			m->nblocked = 0;
			for (i = 0; i < n; i++) {
				__u64 at = max_u64(t, held[i].arrive);

				m->n_blocked++;
				m->blocked_ns_sum += at - held[i].arrive;
				arrive(m, held[i], at);
			}
			continue;
		}

		if (m->ndirty) {
			double bready = 0;
			__u64 t = free_at;
			int d;

			/*
			 * With nobody waiting for it, write-back starts only
			 * once no command has arrived for wb_idle_ms.
			 */
			if (m->wb_idle_ns) {
				t = max_u64(t, wb_since(m));
				if (t > now) {
					m->env.wake(m->env.ctx, t);
					break;
				}
			}
			d = best_dirty(m, t, &bready);
			destage(m, d, bready);
			continue;
		}
		break;
	}
	return now;
}

/* a request reaches the drive at `now` (or is released by a flush then) */
static void arrive(struct hdd_model *m, struct hdd_req r, __u64 now)
{
	r.arrive = now;
	m->last_cmd = now;
	if (m->flush_tag >= 0) {
		m->blocked[m->nblocked++] = r;
		return;
	}

	switch (r.op) {
	case MODEL_READ:
		m->n_read++;
		m->pend[m->npend++] = r;
		break;
	case MODEL_WRITE: {
		int stream = r.lba == m->last_wend;

		m->n_write++;
		m->last_wend = r.lba + r.nr;
		/*
		 * A logging drive with no room does not make a sequential
		 * stream wait for small writes to be written back one by
		 * one: what continues the previous write goes straight to
		 * the platters, as a write-through write does.
		 */
		if (!m->cache_bytes || (m->log && stream &&
					(m->nwait || !cache_room(m, r.nr)))) {
			m->pend[m->npend++] = r;
		} else if (!m->nwait && cache_room(m, r.nr)) {
			dirty_add(m, r.lba, r.nr, now);
			cache_filled(m, r.nr, now);
			m->env.done(m->env.ctx, r.tag,
				    now + host_ns(m, r.nr));
		} else {
			m->n_cache_full++;
			m->wait[m->nwait++] = r;
		}
		break;
	}
	case MODEL_FLUSH:
		if (!m->cache_bytes) {
			m->env.done(m->env.ctx, r.tag, now);
			break;
		}
		/* a logging drive with nothing new to log has nothing to do */
		if (m->log && !m->unlogged && !m->nwait && !m->npend) {
			m->env.done(m->env.ctx, r.tag, now);
			m->n_flush++;
			break;
		}
		m->flush_tag = r.tag;
		m->flush_start = now;
		m->flush_bytes_sum += m->log ? m->unlogged_bytes :
			m->dirty_bytes;
		break;
	default:
		assert(0);
	}
}

/* ---- interface ---------------------------------------------------- */

/*
 * Profiles: named parameter sets. hgst-7k8 approximates the HGST
 * Ultrastar 7K8 (HUS728T8TALE6L4, 8 TB SATA, 7200 rpm) from its spec
 * sheet; barracuda-2t the Seagate BarraCuda ST2000DM006 (2 TB SATA,
 * 7200 rpm), fitted to measurements of a real drive: it seeks slower
 * than its spec sheet says, reorders little, and holds few random
 * writes in its cache. See README for the sources and the calibration
 * status of each number.
 */
static const struct {
	const char *name;
	struct hdd_params p;
} profiles[] = {
	{ "hgst-7k8", {
		.rpm = 7200, .seek_min_ms = 0.6, .seek_avg_ms = 8.0,
		.mbps = 205, .iface_mbps = 600, .iface_us = 30,
		.cache_mb = 64, .ncq = 32, .stroke = 1.0, .max_wait_ms = 500,
	} },
	/*
	 * An 8 TB 7200 rpm nearline SATA drive as measured through three
	 * specimens (model not known; see README): seek and rotation as
	 * hgst-7k8, random writes through a logging cache.
	 */
	{ "nearline-8t", {
		.rpm = 7200, .seek_min_ms = 0.6, .seek_avg_ms = 7.7,
		.mbps = 265, .mbps_inner = 128, .iface_mbps = 600,
		.iface_us = 450, .capacity_gb = 8000, .cache_mb = 85, .cache_ext = 5400,
		.ncq = 32, .stroke = 1.0,
		.max_wait_ms = 500, .wsettle_ms = 0.7,
		.log_ms = 8, .log_us = 70, .wb_idle_ms = 50,
		.wb_turns = 12, .wb_queue = 64,
	} },
	{ "barracuda-2t", {
		.rpm = 7200, .seek_min_ms = 1.0, .seek_avg_ms = 13.0,
		.mbps = 150, .iface_mbps = 600, .iface_us = 30,
		.cache_mb = 1, .ncq = 4, .wb_window = 8, .stroke = 1.0,
		.max_wait_ms = 500,
	} },
};

const struct hdd_params *hdd_profile(const char *name)
{
	unsigned i;

	if (!name)
		return &profiles[0].p;
	for (i = 0; i < sizeof(profiles) / sizeof(profiles[0]); i++)
		if (!strcmp(profiles[i].name, name))
			return &profiles[i].p;
	return NULL;
}

int hdd_params_check(const struct hdd_params *p, __u64 max_io_bytes)
{
	if (p->seek_avg_ms < p->seek_min_ms || p->seek_min_ms < 0 ||
	    p->rpm == 0 || p->mbps <= 0 || p->iface_mbps <= 0 ||
	    p->iface_us < 0 || p->stroke <= 0 || p->ncq == 0 ||
	    p->wb_window > HDD_WB_WINDOW_MAX ||
	    !(p->wsettle_ms >= 0 && p->wsettle_ms <= 1e9) ||
	    !(p->log_ms >= 0 && p->log_ms <= 1e9) ||
	    !(p->log_us >= 0 && p->log_us <= 1e9) ||
	    p->cache_ext > (1U << 30) || p->wb_turns > (1U << 20) ||
	    p->wb_queue > (1U << 20) ||
	    !(p->capacity_gb >= 0 && p->capacity_gb <= 1e9) ||
	    !(p->mbps_inner >= 0 && p->mbps_inner <= p->mbps) ||
	    (p->mbps_inner > 0 && p->stroke > 1) ||
	    (p->capacity_gb > 0 && p->stroke > 1) ||
	    !(p->wb_idle_ms >= 0 && p->wb_idle_ms <= 1e9) ||
	    !(p->max_wait_ms >= 0 && p->max_wait_ms <= 1e9) ||
	    (p->cache_mb && ((__u64)p->cache_mb << 20) < max_io_bytes))
		return -EINVAL;
	return 0;
}

struct hdd_model *hdd_model_new(const struct hdd_params *p, __u64 dev_sectors,
				unsigned depth, const struct model_env *env)
{
	struct hdd_model *m = calloc(1, sizeof(*m));

	assert(m);
	m->p = *p;
	m->env = *env;
	m->depth = depth;
	/* sqrt seek curve: mean over uniform random pairs is 8/15 of full */
	m->seek_full_ms = p->seek_min_ms +
		(p->seek_avg_ms - p->seek_min_ms) * 15.0 / 8.0;
	m->dev_sectors = dev_sectors;
	m->cache_bytes = (__u64)p->cache_mb << 20;
	m->pend = calloc(depth, sizeof(*m->pend));
	m->wait = calloc(depth, sizeof(*m->wait));
	m->blocked = calloc(depth, sizeof(*m->blocked));
	m->spare = calloc(depth, sizeof(*m->spare));
	m->cap_dirty = 1024;
	m->dirty = calloc(m->cap_dirty, sizeof(*m->dirty));
	assert(m->pend && m->wait && m->blocked && m->spare && m->dirty);
	m->flush_tag = -1;
	m->period_ns = 60e9 / p->rpm;
	m->spt = p->mbps * 1e6 * (60.0 / p->rpm) / 512;
	m->tspt = m->spt;
	if (p->capacity_gb > 0)	/* the device is `stroke` of that drive */
		m->tspt = m->spt * dev_sectors /
			(p->capacity_gb * 1e9 / 512 * p->stroke);
	if (m->tspt > m->spt)
		m->tspt = m->spt;
	/* the device is the outer `stroke` of the drive */
	if (p->mbps_inner > 0 && p->mbps_inner < p->mbps)
		m->za = (1 - (p->mbps_inner / p->mbps) *
			 (p->mbps_inner / p->mbps)) * p->stroke / dev_sectors;
	m->max_wait_ns = (__u64)(p->max_wait_ms * 1e6);
	m->wb_idle_ns = (__u64)(p->wb_idle_ms * 1e6);
	m->log = p->cache_mb && (p->log_ms > 0 || p->log_us > 0);
	m->phase0 = (double)(p->seed % 1000003) / 1000003.0;
	return m;
}

void hdd_model_free(struct hdd_model *m)
{
	if (!m)
		return;
	free(m->pend);
	free(m->wait);
	free(m->blocked);
	free(m->spare);
	free(m->dirty);
	free(m);
}

void hdd_model_submit(struct hdd_model *m, int tag, int op, __u64 lba,
		      __u64 nr)
{
	struct hdd_req r = { .tag = tag, .op = op, .lba = lba, .nr = nr };

	/* catch up first, so a request never meets a flush already done */
	arrive(m, r, pump(m));
	pump(m);
}

void hdd_model_wake(struct hdd_model *m)
{
	pump(m);
}

int hdd_model_stats(const struct hdd_model *m, char *buf, int len)
{
	return snprintf(buf, len,
		"reads %llu\nwrites %llu\nflushes %llu\nflush_ms_sum %.3f\n"
		"flush_ms_max %.3f\nflush_dirty_mb_sum %.3f\ndestaged %llu\n"
		"cache_full_waits %llu\nblocked_by_flush %llu\n"
		"blocked_ms_sum %.3f\ndirty_mb %.3f\nlogging_flushes %llu\n"
		"dirty_extents %d\n",
		(unsigned long long)m->n_read, (unsigned long long)m->n_write,
		(unsigned long long)m->n_flush, m->flush_ns_sum / 1e6,
		m->flush_ns_max / 1e6, m->flush_bytes_sum / 1048576.0,
		(unsigned long long)m->n_destage,
		(unsigned long long)m->n_cache_full,
		(unsigned long long)m->n_blocked, m->blocked_ns_sum / 1e6,
		m->dirty_bytes / 1048576.0,
		(unsigned long long)m->n_logged, m->ndirty);
}
