# Guide: running ublk-disksim

How to build it, bring up a modelled disk, put software on it, read what
the model did, and fit it to a drive you care about.

## 1. Requirements

- Linux with the `ublk_drv` module (6.0+; developed on 6.17). Check:
  `modinfo ublk_drv`.
- liburing headers: `apt install liburing-dev` (Debian/Ubuntu) or
  `dnf install liburing-devel`.
- root, for `/dev/ublk-control` and for creating the RAM backing device.
- fio and python3, for `bench/calibrate.sh` and `bench/calibrate_ssd.sh`.

## 2. Build

```bash
make            # produces ./kublk
make check      # model tests on a virtual clock (no root, no ublk)
./kublk help    # all targets and their options
```

`stripe.c` is not built (it needs io_uring opcodes newer than liburing
2.5); everything else from the kernel selftest server is. A binary built
on a newer distribution needs that distribution's glibc (Ubuntu 24.04's
needs 2.38, which 22.04 doesn't have): `make LDFLAGS=-static` links
glibc and liburing into it, and the result runs on older ones. The
kernel's ublk driver ignores parameter types it doesn't know (the DMA
alignment the targets set is only known from linux 6.15 on, and is the
default anyway). How the pieces fit together:
[ARCHITECTURE.md](ARCHITECTURE.md).

## 3. Bring up a disk

The model keeps no data of its own: it passes every read and write to a
backing device and only delays the completion. Use something fast that
accepts `O_DIRECT` I/O with 512-byte alignment, normally RAM.

**Backing store, option A: null_blk (memory backed).**

```bash
sudo modprobe null_blk nr_devices=0
sudo mount -t configfs none /sys/kernel/config 2>/dev/null || true
d=/sys/kernel/config/nullb/simback0
sudo mkdir $d
echo 8192 | sudo tee $d/size            # MiB
echo 512  | sudo tee $d/blocksize
echo 1    | sudo tee $d/memory_backed
echo 2    | sudo tee $d/queue_mode
echo 1    | sudo tee $d/power           # -> /dev/simback0
```

**Option B: brd.** `sudo modprobe brd rd_nr=1 rd_size=$((8*1024*1024))`
gives `/dev/ram0` (size in KiB).

Memory is only used as data is written.

**Start the model.**

```bash
sudo modprobe ublk_drv
sudo ./kublk add -t hdd -n 0 -q 1 -d 32 --profile hgst-7k8 \
     --stats /tmp/ublk0.stats /dev/simback0
# -> /dev/ublkb0
cat /sys/block/ublkb0/queue/rotational    # 1
cat /sys/block/ublkb0/queue/write_cache   # "write back" (cache on)
```

- `-q 1`: one hardware queue. Required: one actuator, one thread.
- `-d 32`: queue depth, as SATA NCQ.
- `-n N`: device id, gives `/dev/ublkbN`. Omit to get the next free one.
- Model options come after the common ones, each as `--name value`
  (table in the README). `--profile` is applied first; anything else
  overrides it, in any order.

**Write cache off** (like `hdparm -W0`, or a controller that hides the
cache):

```bash
sudo ./kublk add -t hdd -n 1 -q 1 -d 32 --cache_mb 0 /dev/simback1
cat /sys/block/ublkb1/queue/write_cache   # "write through"
```

The kernel then never sends flushes to it, and every write pays the
mechanical cost before it completes.

**List and remove.**

```bash
sudo ./kublk list
sudo ./kublk del -n 0          # or: del --all
echo 0 | sudo tee /sys/kernel/config/nullb/simback0/power
sudo rmdir /sys/kernel/config/nullb/simback0
```

The server runs as a daemon per device; `del` stops it and removes the
device. Without the recovery options (`kublk help`), a killed daemon
means failed I/O on the device.

## 4. Put software on it

It is a normal block device:

```bash
sudo fio --name=t --filename=/dev/ublkb0 --direct=1 --ioengine=libaio \
         --rw=randread --bs=4k --iodepth=1 --runtime=30 --time_based
sudo mkfs.xfs /dev/ublkb0 && sudo mount /dev/ublkb0 /mnt
```

Anything that tunes itself by `rotational` (the kernel's I/O scheduler
choice, databases, storage daemons) sees a spinning disk. `chown` the
device if the software runs unprivileged.

Stacking works as usual: device-mapper targets (e.g. `dm-log-writes` for
crash-consistency tests), LVM, md.

## 5. Read what the model did

With `--stats FILE`, the model rewrites FILE once a second:

| field | meaning |
|---|---|
| `reads`, `writes` | requests seen |
| `flushes` | cache flushes completed |
| `flush_ms_sum`, `flush_ms_max` | time from flush arrival to completion |
| `flush_dirty_mb_sum` | dirty data present when flushes arrived |
| `destaged` | write-back operations (at most one track each) |
| `cache_full_waits` | writes that had to wait for cache space |
| `blocked_by_flush`, `blocked_ms_sum` | requests held back by a flush in progress, and for how long |
| `dirty_mb` | dirty data right now |
| `completions` | requests completed (both targets) |
| `late_us_p50`, `late_us_p99`, `late_us_p999`, `late_us_max` | how late completions fired against the model's schedule, µs (both targets) |

The `late_us_*` lines say how faithfully this host delivers the model's
timing; the model's own numbers are exact, the host only adds this.
Quantiles are the upper edges of 1/16-octave buckets (at most ~4.4%
high). A p99 of tens of µs is normal; hundreds of µs on an SSD profile
means the server thread is at its limit or the host is busy. See
[VALIDATION.md](VALIDATION.md).

The kernel's own counters for the device are in
`/sys/block/ublkbN/stat`; field 16 is flushes completed, field 17 the
time spent in them (ms).

## 6. Calibrate

`bench/calibrate.sh` creates a fresh 4 GiB null_blk-backed device, runs
a fixed set of fio jobs and prints one line per job:

```bash
sudo bench/calibrate.sh 30                       # 30 s per job, profile default
sudo bench/calibrate.sh 30 --cache_mb 0          # same, cache off
sudo ID=11 OUT=/tmp/cal bench/calibrate.sh 60 --seek_avg_ms 8.5
```

Jobs: 4K random read at QD1, 2, 4, 8, 16 and 32, 1M sequential read, 1M sequential
write at QD1 and QD4, 4K random write without and with an fsync per
write, one flush of whatever the random writes left in the cache, a
reader next to a flushing writer, readers at QD4 next to a cached writer
and a reader next to a sequential writer (each followed by a timed
flush). Raw fio JSON and the model's stats go to `$OUT`. The backing is
named after the device id (`ublksim<ID>`), so runs with different `ID`s
don't collide.

To imitate another drive:

1. Get its numbers: the spec sheet (RPM, average seek, sustained
   transfer, buffer size), and if you can, the same fio jobs run on the
   real drive. On a running system, `/proc/diskstats` (or node_exporter's
   `node_disk_flush_requests_*`) gives the mean flush time under real
   load.
2. Set `--rpm`, `--seek_avg_ms`, `--mbps`; `--cache_mb` for the write
   cache (vendors rarely publish how much of the buffer caches writes).
3. Run `calibrate.sh` and compare: random QD1 checks seek + rotation,
   sequential checks the transfer rate, random QD32 checks reordering,
   write + fsync checks the flush path.
4. Add a profile to `profiles[]` in `hdd_model.c` once it fits, and its
   expected values to `bench/expect/<profile>.tsv`: with a stock
   profile, `calibrate.sh` checks the run against that file and exits
   non-zero if a number is out of tolerance.

`--stroke` scales seek distances: a small device models a small span of
a big disk. Leave it at 1.0 if the software spreads data over the whole
device.

## 7. Limits and gotchas

- One thread per device: ~200K 4K IOPS and ~4 GB/s at most.
- No data persistence: the backing is RAM. Power-loss behaviour is not
  modelled; for crash tests, stack `dm-log-writes` on top and replay.
  `bench/integrity.sh` checks that data reads back as written on every
  target and cache mode.
- One transfer rate and one track size for the whole disk (no zones), no
  read cache beyond sequential read-ahead, no firmware cap on dirty data
  (on the 4 GiB calibration device, a flush after filling 64 MiB with
  random writes takes ~11–15 s; a bigger device spreads the writes and
  takes longer).
- The model is only as good as its calibration; say which profile and
  parameters produced a number when you report it.

## 8. Troubleshooting

| symptom | cause |
|---|---|
| `can't open /dev/ublk-control` | `modprobe ublk_drv`, run as root |
| `cmd_dev_add: command failed` | the target refused its options; the daemon's messages are lost, so rerun with `--foreground` to see which |
| `hdd: one actuator, needs -q 1` | pass `-q 1` and no `--nthreads` |
| `ssd: one model thread, needs -q 1` | same for the ssd target |
| `ssd: SATA NCQ holds 32 commands` | pass `-d 32`, or `--iface nvme` for a deeper queue |
| `too many target options` | kublk takes at most 15 target options: start from the nearest `--profile` and override only what differs |
| I/O errors with a 4K-block backing device | the model is 512e: use a backing store with 512-byte blocks |
| build fails on `IORING_OP_*` | liburing too old; the Makefile already leaves out `stripe.c` |

## 9. The ssd target

Same setup as above, with `-t ssd` and the queue depth of the interface:

```bash
sudo ./kublk add -t ssd -n 2 -q 1 -d 32 --profile sata-plp \
     --floor_us 24 --stats /tmp/ublk2.stats /dev/simback2
sudo ./kublk add -t ssd -n 3 -q 1 -d 128 --profile nvme-plp \
     --floor_us 24 /dev/simback3
cat /sys/block/ublkb2/queue/rotational    # 0
cat /sys/block/ublkb3/queue/write_cache   # "write through" (vwc 0)
```

**The floor.** A request through ublk costs the host some 20–40 µs
before any model delay: two trips between kernel and server, and the
server thread waking up for its completion timer. That is noise for a
disk and a third of an SSD read, so the ssd target subtracts
`--floor_us` from every completion, never finishing a request before it
arrived. Measure it on the host you run on: `bench/calibrate_ssd.sh`
does (4K random read at QD1 through the ssd target set to a fixed
100 µs, minus 100 µs, minus the same read on the null_blk) and prints
it; use that number for your own devices. A device faster than the
floor can't be modelled: an NVMe drive that acks a write in 15 µs comes
out at the floor.

In a KVM guest the wakeup is a vCPU leaving HLT: about 35 µs, and more
for sleeps beyond ~100 µs. Guest halt polling
(`sudo modprobe cpuidle-haltpoll force=1`, check
`/sys/devices/system/cpu/cpuidle/current_driver`) brought it to about
18 µs on the machine this was developed on; sleeps of 200–400 µs still
come out 10–40 µs long.

**Profiles.** `sata-plp`, `nvme-plp`, `sata-consumer`, `micron-7300`
(README). The difference that matters for most software is the flush:
free with power-loss protection, milliseconds without it.

**Stats** (`--stats FILE`):

| field | meaning |
|---|---|
| `reads`, `writes`, `flushes` | requests seen (flushes: only with `vwc 1`) |
| `flush_ms_sum`, `flush_ms_max` | time from flush arrival to completion |
| `seq_write_mb`, `random_write_mb` | writes classified as continuing a stream or not |
| `program_units` | page programs done, garbage collection included |
| `buffer_full_waits` | writes that waited for buffer space |
| `blocked_by_flush`, `blocked_ms_sum` | requests held back by a SATA flush |
| `read_die_waits`, `read_die_wait_ms_sum` | reads that found their die busy (another read or a program) |
| `read_suspends` | reads that suspended a program (`susp_us` > 0) |
| `buffer_mb` | data in the write buffer right now |

**Calibrate:**

```bash
sudo bench/calibrate_ssd.sh 30 --profile sata-consumer
sudo FLOOR_US=24 bench/calibrate_ssd.sh 30 --profile nvme-plp --waf 4
# the same jobs on a real drive, judged by the same expectations
# (writes to its first 4 GiB; refuses devices in use)
sudo REAL=/dev/nvme0n1p1 bench/calibrate_ssd.sh 30 --profile micron-7300
```

Jobs: 4K random read at QD1, QD32 (and QD128 for NVMe), 128K sequential
read and write at QD32, 4K random write at QD1 and QD32, 4K random and
sequential writes with an fsync after each, a reader next to a
fsyncing writer, and a QD16 random writer keeping the buffer full next
to a fsyncing writer. Latencies are fio's submission to completion
(`lat`); the per-job fio output is json+ with the full histograms, which
`bench/figures.py` turns into the figures in `docs/img/` (see its
README there).

To imitate another SSD: from the spec sheet, fit `iface_us` to the QD1
write latency, `tr_us` to the QD1 read latency, `cmd_us` (SATA) to the
QD32 random read IOPS, `dies` · page / (`tprog_us` + page / `ch_mbps`)
to the sequential write rate, and `waf` to the steady-state (full drive) random write
IOPS. `flush_us` comes from a measured 4K write + fsync at QD1, which
Ceph users publish for many drives. Start from the nearest `--profile`
and override what differs (kublk takes at most 15 target options), then
add a profile to `profiles[]` in `ssd_model.c` and its expected values
to `bench/expect/<profile>.tsv`.

Limits: a first-order model. No garbage collection as a process (the
`waf` factor charges it to the writes that cause it, at steady state),
no SLC cache, program suspend only with `--susp_us` (0 in the profiles:
a read behind a program waits for it) and with no limit on suspends per
program,
no reads from the write buffer, one model thread (~190K 4K IOPS and
~4 GB/s on a current server, below NVMe drives' peak).
