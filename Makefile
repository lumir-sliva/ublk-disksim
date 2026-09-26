# SPDX-License-Identifier: GPL-2.0
# Standalone build of kublk (linux tools/testing/selftests/ublk) plus the
# hdd and ssd timing targets. Needs liburing headers (liburing-dev).

CFLAGS ?= -O2 -g
CFLAGS += -Wall -D_GNU_SOURCE -Iinclude
LDLIBS += -lpthread -lm -luring

# stripe.c is left out: it needs IORING_OP_READV_FIXED (linux 6.15 uapi),
# newer than the io_uring headers of Ubuntu 24.04's liburing 2.5
SRCS := kublk.c null.c file_backed.c common.c fault_inject.c hdd.c ssd.c
HDRS := kublk.h utils.h ublk_dep.h include/linux/ublk_cmd.h

kublk: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDLIBS)

clean:
	rm -f kublk

.PHONY: clean
