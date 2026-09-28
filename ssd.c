// SPDX-License-Identifier: GPL-2.0
/*
 * ssd: kublk target for the flash SSD timing model (ssd_model.c), SATA or
 * NVMe, on top of a backing device (normally RAM). Data goes to the
 * backing device at once; the model decides when each request completes.
 * All state lives in one thread: the target requires -q 1 and one thread.
 */

#include "model_kublk.h"
#include "ssd_model.h"

struct ssd_dev {
	struct mk k;
	struct ssd_model *m;
};

static void ssd_stats(struct ssd_dev *d)
{
	char buf[1024];

	if (!mk_stats_due(&d->k))
		return;
	ssd_model_stats(d->m, buf, sizeof(buf));
	mk_stats_write(&d->k, buf);
}

static int ssd_queue_io(struct ublk_thread *t, struct ublk_queue *q, int tag)
{
	struct ssd_dev *d = q->dev->private_data;
	__u64 lba, nr;
	int op;

	if (mk_queue_io(&d->k, t, q, tag, &op, &lba, &nr))
		return 0;
	ssd_model_submit(d->m, tag, op, lba, nr);
	ssd_stats(d);
	return 0;
}

static void ssd_io_done(struct ublk_thread *t, struct ublk_queue *q,
			const struct io_uring_cqe *cqe)
{
	struct ssd_dev *d = q->dev->private_data;

	if (mk_io_done(&d->k, t, q, cqe)) {
		ssd_model_wake(d->m);
		ssd_stats(d);
	}
}

static int ssd_init_tgt(const struct dev_ctx *ctx, struct ublk_dev *dev)
{
	const struct ssd_params *p = &ctx->ssd;
	unsigned depth = dev->dev_info.queue_depth;
	struct ssd_dev *d;
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
	if (!p->nvme && depth > 32) {
		ublk_err("ssd: SATA NCQ holds 32 commands: use -d 32 "
			 "(or --iface nvme)\n");
		return -EINVAL;
	}
	if (ssd_params_check(p, dev->dev_info.max_io_buf_bytes, depth)) {
		ublk_err("ssd: bad model parameters (buf_mb must hold the "
			 "largest request plus a page; see `kublk help`)\n");
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
	dev->tgt.sq_depth = dev->tgt.cq_depth = mk_ring_depth(depth);

	d = calloc(1, sizeof(*d));
	mk_init(&d->k, p->stats);
	d->m = ssd_model_new(p, bytes >> 9, depth, &d->k.env);
	dev->private_data = d;

	ublk_log("ssd: %s link %.0f MB/s cmd %.1f us latency %.1f us, %u dies, "
		 "page %u KiB, read %.1f us + %.1f us a level, program %.1f us "
		 "x waf %.2f, suspend %.1f us, channel %.0f MB/s, buffer %u MiB, "
		 "plp %u vwc %u flush %.1f us, floor %.1f us, history %s, "
		 "gc pool %.0f MiB at %.0f MB/s\n",
		 p->nvme ? "nvme" : "sata", p->iface_mbps, p->cmd_us,
		 p->iface_us, p->dies, p->page_kb, p->tr_us, p->tr_step_us,
		 p->tprog_us, p->waf, p->susp_us, p->ch_mbps, p->buf_mb, p->plp,
		 p->vwc, p->flush_us, p->floor_us, p->history ? "rnd" : "seq",
		 p->gc_pool_mb, p->gc_mbps);
	return 0;
}

static void ssd_deinit_tgt(struct ublk_dev *dev)
{
	struct ssd_dev *d = dev->private_data;
	char buf[1024];

	backing_file_tgt_deinit(dev);
	if (!d)
		return;
	ssd_model_stats(d->m, buf, sizeof(buf));
	mk_stats_write(&d->k, buf);
	mk_close(&d->k);
	ssd_model_free(d->m);
	free(d);
}

static void ssd_cmd_line(struct dev_ctx *ctx, int argc, char *argv[])
{
	struct ssd_params *p = &ctx->ssd;
	int i;

	*p = *ssd_profile(NULL);
	/* profile first, so explicit parameters override it in any order */
	for (i = 1; i + 1 < argc; i += 2)
		if (!strcmp(argv[i], "--profile")) {
			const struct ssd_params *pp = ssd_profile(argv[i + 1]);

			if (!pp) {
				ublk_err("ssd: unknown profile %s\n", argv[i + 1]);
				exit(EXIT_FAILURE);
			}
			*p = *pp;
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
		else if (!strcmp(k, "--tr_step_us"))
			p->tr_step_us = strtod(v, NULL);
		else if (!strcmp(k, "--ch_mbps"))
			p->ch_mbps = strtod(v, NULL);
		else if (!strcmp(k, "--tprog_us"))
			p->tprog_us = strtod(v, NULL);
		else if (!strcmp(k, "--waf"))
			p->waf = strtod(v, NULL);
		else if (!strcmp(k, "--susp_us"))
			p->susp_us = strtod(v, NULL);
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
		else if (!strcmp(k, "--history")) {
			if (strcmp(v, "seq") && strcmp(v, "rnd")) {
				ublk_err("ssd: --history seq or rnd\n");
				exit(EXIT_FAILURE);
			}
			p->history = !strcmp(v, "rnd");
		} else if (!strcmp(k, "--gc_pool_mb"))
			p->gc_pool_mb = strtod(v, NULL);
		else if (!strcmp(k, "--gc_mbps"))
			p->gc_mbps = strtod(v, NULL);
		else if (!strcmp(k, "--stats"))
			snprintf(p->stats, sizeof(p->stats), "%s", v);
		/* other targets' options pass through here too: ignore */
	}
}

static void ssd_usage(const struct ublk_tgt_ops *ops)
{
	printf("\tssd: [--profile sata-plp|nvme-plp|sata-consumer|micron-7300]\n"
	       "\t     [--iface sata|nvme]"
	       " [--iface_mbps X] [--cmd_us X] [--iface_us X]\n"
	       "\t     [--dies N] [--page_kb N]"
	       " [--tr_us X] [--tr_step_us X] [--ch_mbps X] [--tprog_us X]\n"
	       "\t     [--waf X] [--susp_us X] [--buf_mb N]"
	       " [--plp 0|1] [--vwc 0|1] [--flush_us X]\n"
	       "\t     [--floor_us X] [--history seq|rnd]"
	       " [--gc_pool_mb X] [--gc_mbps X] [--stats FILE]\n"
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
