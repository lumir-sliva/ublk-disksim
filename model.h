/* SPDX-License-Identifier: MIT */
/* SPDX-FileCopyrightText: 2026 Lumir Sliva */
/*
 * What a timing model sees of the world. The models (hdd_model.c,
 * ssd_model.c) never touch io_uring or the clock directly: the kublk glue
 * (model_kublk.c) provides these calls on a real device, the tests
 * (tests/model_test.c) on a virtual clock.
 */
#ifndef MODEL_H
#define MODEL_H

#include <linux/types.h>

enum { MODEL_READ, MODEL_WRITE, MODEL_FLUSH };

struct model_env {
	void *ctx;
	/* current time, ns, monotonic */
	__u64 (*now)(void *ctx);
	/* request `tag` completes at `when` (ns; may be in the past) */
	void (*done)(void *ctx, int tag, __u64 when);
	/*
	 * call the model's wake function at `when` or later. The model asks
	 * again for its next event after every call into it, so an
	 * implementation may keep only the earliest pending wake-up.
	 */
	void (*wake)(void *ctx, __u64 when);
};

#endif
