# SPDX-License-Identifier: GPL-2.0
# Standalone build of kublk (linux tools/testing/selftests/ublk) plus the
# hdd and ssd timing targets. Needs liburing headers (liburing-dev).
# `make check` builds and runs the model tests (no root, no ublk needed).

CFLAGS ?= -O2 -g
CFLAGS += -Wall -D_GNU_SOURCE -Iinclude
LDLIBS += -lpthread -lm -luring

MODEL_SRCS := hdd_model.c ssd_model.c
MODEL_HDRS := model.h hdd_model.h ssd_model.h

# stripe.c is left out: it needs IORING_OP_READV_FIXED (linux 6.15 uapi),
# newer than the io_uring headers of Ubuntu 24.04's liburing 2.5
SRCS := kublk.c null.c file_backed.c common.c fault_inject.c \
	model_kublk.c hdd.c ssd.c $(MODEL_SRCS)
HDRS := kublk.h utils.h ublk_dep.h include/linux/ublk_cmd.h model_kublk.h \
	$(MODEL_HDRS)

kublk: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

tests/model_test: tests/model_test.c $(MODEL_SRCS) $(MODEL_HDRS)
	$(CC) $(CFLAGS) -DMODEL_CHECK_SPTF -I. -o $@ tests/model_test.c \
		$(MODEL_SRCS) -lm

check: tests/model_test
	./tests/model_test

clean:
	rm -f kublk tests/model_test

.PHONY: check clean
