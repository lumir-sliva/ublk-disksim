// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Lumir Sliva
/*
 * hdd: kublk target for the hard disk timing model (hdd_model.c), on top
 * of a backing device (normally RAM). Data goes to the backing device at
 * once; the model decides when each request completes. All state lives in
 * one thread: the target requires -q 1 and one thread.
 */

#include "model_kublk.h"
#include "hdd_model.h"

struct hdd_dev {
	struct mk k;
	struct hdd_model *m;
};

static void hdd_stats(struct hdd_dev *d)
{
	char buf[1024];

	if (!mk_stats_due(&d->k))
		return;
	hdd_model_stats(d->m, buf, sizeof(buf));
	mk_stats_write(&d->k, buf);
}

static int hdd_queue_io(struct ublk_thread *t, struct ublk_queue *q, int tag)
{
	struct hdd_dev *d = q->dev->private_data;
	__u64 lba, nr;
	int op;

	if (mk_queue_io(&d->k, t, q, tag, &op, &lba, &nr))
		return 0;
	hdd_model_submit(d->m, tag, op, lba, nr);
	hdd_stats(d);
	return 0;
}

static void hdd_io_done(struct ublk_thread *t, struct ublk_queue *q,
			const struct io_uring_cqe *cqe)
{
	struct hdd_dev *d = q->dev->private_data;

	if (mk_io_done(&d->k, t, q, cqe)) {
		hdd_model_wake(d->m);
		hdd_stats(d);
	}
}

static int hdd_init_tgt(const struct dev_ctx *ctx, struct ublk_dev *dev)
{
	const struct hdd_params *p = &ctx->hdd;
	unsigned depth = dev->dev_info.queue_depth;
	struct hdd_dev *d;
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
	if (hdd_params_check(p, dev->dev_info.max_io_buf_bytes)) {
		ublk_err("hdd: bad model parameters (see `kublk help`)\n");
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
	dev->tgt.sq_depth = dev->tgt.cq_depth = mk_ring_depth(depth);

	d = calloc(1, sizeof(*d));
	mk_init(&d->k, p->stats);
	d->m = hdd_model_new(p, bytes >> 9, depth, &d->k.env);
	dev->private_data = d;

	ublk_log("hdd: rpm %u seek %.2f/%.2f/%.2f ms (min/avg/full) media %.0f MB/s "
		 "(%.0f sectors/track) cache %u MiB ncq %u wb window %u "
		 "max wait %.0f ms stroke %.2f\n",
		 p->rpm, p->seek_min_ms, p->seek_avg_ms, d->m->seek_full_ms,
		 p->mbps, d->m->spt, p->cache_mb, p->ncq, p->wb_window,
		 p->max_wait_ms, p->stroke);
	return 0;
}

static void hdd_deinit_tgt(struct ublk_dev *dev)
{
	struct hdd_dev *d = dev->private_data;
	char buf[1024];

	backing_file_tgt_deinit(dev);
	if (!d)
		return;
	hdd_model_stats(d->m, buf, sizeof(buf));
	mk_stats_write(&d->k, buf);
	mk_close(&d->k);
	hdd_model_free(d->m);
	free(d);
}

static void hdd_cmd_line(struct dev_ctx *ctx, int argc, char *argv[])
{
	struct hdd_params *p = &ctx->hdd;
	int i;

	*p = *hdd_profile(NULL);
	/* profile first, so explicit parameters override it in any order */
	for (i = 1; i + 1 < argc; i += 2)
		if (!strcmp(argv[i], "--profile")) {
			const struct hdd_params *pp = hdd_profile(argv[i + 1]);

			if (!pp) {
				ublk_err("hdd: unknown profile %s\n", argv[i + 1]);
				exit(EXIT_FAILURE);
			}
			*p = *pp;
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
		else if (!strcmp(k, "--wb_window"))
			p->wb_window = strtoul(v, NULL, 10);
		else if (!strcmp(k, "--max_wait_ms"))
			p->max_wait_ms = strtod(v, NULL);
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
	printf("\thdd: [--profile hgst-7k8|barracuda-2t] [--rpm N] "
	       "[--seek_min_ms X] [--seek_avg_ms X]\n"
	       "\t     [--mbps X] [--iface_mbps X] [--iface_us X] "
	       "[--cache_mb N (0 = write-through)]\n"
	       "\t     [--ncq N] [--wb_window N (0 = whole cache, max 64)] "
	       "[--max_wait_ms X (0 = none)]\n"
	       "\t     [--stroke F] [--seed N] [--stats FILE] "
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
