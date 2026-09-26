# ublk-disksim

Timing models of real disks as Linux block devices, for benchmarking
software against a "disk" you don't have. Built on
[ublk](https://docs.kernel.org/block/ublk.html) and the kernel's `kublk`
selftest server (linux v6.17, `tools/testing/selftests/ublk`).

Data is stored in a RAM backing device at full speed; the model only
decides *when* each request completes. So a filesystem or database on top
behaves functionally like on RAM, but sees the latency, throughput and
queueing of the modelled disk.

## Targets

### `hdd`: single-actuator hard disk

What it models (see the header of `hdd_model.c`):

- one actuator: requests are served one at a time,
  shortest-positioning-time-first among the oldest `ncq` waiting ones
  (NCQ with rotational position ordering), except that a request passed
  over for `max_wait_ms` goes next;
- positioning = seek (sqrt curve over the distance, none within a track) +
  waiting for the sector to rotate under the head (sector angle from the
  LBA, platter phase from the clock), then media transfer; a read, a
  write-back or an already queued write starting where the head is pays
  no positioning (a write-through write that arrives after the previous
  one finished waits for its sector to come round);
- volatile write cache: writes complete after the host transfer and are
  written back one track at most per operation, when the actuator is
  idle, and once the cache is 3/4 full or writes wait for space also in
  turn with queued requests (one write-back per request served, one that
  started while the queue was empty counts); a full cache makes writes
  wait;
- FLUSH as SATA `FLUSH CACHE`: non-queued. Queued requests finish, the
  whole cache is destaged, and everything that arrives meanwhile, reads
  included, waits;
- `--cache_mb 0` = write-through (write cache off, or a controller that
  hides it): no volatile cache is advertised, so the kernel sends no
  flushes, and every write pays mechanical time.

Not modelled: zoned transfer rates (one rate, one track size), read cache
and read-ahead beyond "sequential costs nothing", firmware limits on dirty
data, destage idle timers. The 3/4 threshold and the one-for-one turns of
forced write-back, and the 500 ms age limit, are assumptions; drives
don't publish theirs.

Parameters (`kublk add -t hdd ... --<name> <value>`):

| option | meaning | `hgst-7k8` |
|---|---|---|
| `--profile` | named parameter set, applied first | |
| `--rpm` | spindle speed | 7200 |
| `--seek_min_ms` | track-to-track seek | 0.6 |
| `--seek_avg_ms` | average random seek | 8.0 |
| `--mbps` | media transfer rate | 205 |
| `--iface_mbps` | host link rate (cached writes) | 600 |
| `--iface_us` | per-command overhead (cached writes) | 30 |
| `--cache_mb` | volatile write cache, 0 = write-through | 64 |
| `--ncq` | requests considered for reordering | 32 |
| `--max_wait_ms` | a request passed over this long goes next, 0 = no limit | 500 |
| `--stroke` | fraction of the full stroke the device spans | 1.0 |
| `--seed` | platter phase at clock zero | 0 |
| `--stats` | file rewritten once a second with model counters | |

`hgst-7k8` follows the HGST/WD Ultrastar 7K8 (HUS728T8TALE6L4, 8 TB SATA)
spec sheet: 8 ms average seek, 4.16 ms average latency, 205 MB/s internal
rate, 6 Gb/s SATA, 256 MB buffer. How much of the buffer caches writes
isn't published; 64 MiB is an assumption.

Calibration of `hgst-7k8` (`bench/calibrate.sh 30`, kernel 6.17, 4 GiB
device, so seeks span the full stroke; one run, the latest checked run
is in [docs/VALIDATION.md](docs/VALIDATION.md)):

| test | model | reference |
|---|---|---|
| 4K random read QD1 | 80 IOPS, 12.5 ms mean | 8.0 + 4.16 ms = 12.2 ms (spec) |
| 4K random read QD32 | 198 IOPS, p99 518 ms (age limit) | ~200 for 7200 rpm SATA with NCQ (typical) |
| 1M sequential read QD1 | 186–197 MB/s | 205 MB/s internal, 255 MB/s outer (spec) |
| 1M sequential write QD1 / QD4, cache on | 207 / 206 MB/s, QD4 p99 25 ms | |
| 1M sequential write QD1 / QD4, cache off | 78 / 205 MB/s | QD1 misses a revolution per write |
| 4K random write + fsync | 80/s, flush 12.4 ms | 13–16 ms median per flush on these drives in production, at 0.4–5 flushes/s with ~4 larger writes per flush |
| flush of a full 64 MiB cache of random 4K writes | 11–15 s | unknown |
| 4K read QD1 next to a write+fsync QD1 job | 41 IOPS, 24.5 ms p50 | reads wait for the flush |
| 4K read QD4 next to a 4K random writer QD1, cache on | reads 106 IOPS; writes 653 IOPS, p99 14 ms (cache stays full) | |
| 4K read QD1 next to a 1M sequential writer QD4, cache on / off | reads 30 IOPS, p50 33 ms; writes 53 MB/s / reads 2 IOPS, p99 522 ms (age limit); writes 191 MB/s | depends on how a drive shares time between them (assumed one-for-one, 500 ms age limit) |
| cache off, 4K random write QD1 | 80 IOPS, 0 flushes at the device | |

### `ssd`: flash SSD, SATA or NVMe

What it models (see the header of `ssd_model.c`):

- flash dies with their own timelines: a page read takes `tr_us` on its
  die (die = page number mod `dies`) plus the channel transfer, and waits
  for whatever that die is doing, a program included;
- a host link (`iface_mbps`, plus `cmd_us` of link time per command; on
  SATA that command rate caps 4K random reads near 100K IOPS, the NVMe
  profile sets it to 0) and a controller latency `iface_us` per command;
- a write buffer: writes complete once buffered; full pages are
  programmed on whichever die is free first. Steady-state garbage
  collection is folded in: a page of random writes costs `waf` program
  units, a page of writes continuing a recent stream costs one;
- FLUSH: with power-loss protection (`--plp 1`) the buffer is durable and
  a flush costs `flush_us`; without it the flush waits until everything
  buffered is programmed, plus `flush_us`. A flush with nothing written
  since the previous one is free. SATA flushes are non-queued, everything
  behind them waits (SATA also means NCQ: at most 32 commands, `-d 32`);
  NVMe flushes are queued. `--vwc 0` advertises no volatile cache, so the
  kernel sends no flushes, as for NVMe drives that report none;
- `--floor_us`: the host's own ublk overhead, subtracted from every
  completion so that the parameters are device latencies (docs/GUIDE.md).

Not modelled: garbage collection as a process (idle-time GC, fill level),
SLC caching, program/erase suspend, reads from the write buffer, mapping
table misses, TRIM, multiple NVMe queues.

| option | meaning | `sata-plp` | `nvme-plp` | `sata-consumer` |
|---|---|---|---|---|
| `--profile` | named parameter set, applied first | | | |
| `--iface` | `sata` or `nvme` | sata | nvme | sata |
| `--iface_mbps` | host link rate | 560 | 6900 | 560 |
| `--cmd_us` | link time per command | 3 | 0 | 3 |
| `--iface_us` | controller latency per command | 29 | 14 | 17.5 |
| `--dies` | flash dies | 32 | 64 | 16 |
| `--page_kb` | program unit | 16 | 16 | 16 |
| `--tr_us` | page read, incl. ECC and lookup | 75 | 62 | 46 |
| `--ch_mbps` | flash channel rate | 800 | 1200 | 1200 |
| `--tprog_us` | page program | 700 | 400 | 390 |
| `--waf` | program units per page of random writes | 7.3 | 4.9 | 13.7 |
| `--buf_mb` | write buffer | 32 | 64 | 32 |
| `--plp` | power-loss protection | 1 | 1 | 0 |
| `--vwc` | advertise a volatile write cache | 1 | 0 | 1 |
| `--flush_us` | flush cost once drained | 15 | 0 | 3200 |
| `--floor_us` | host overhead to subtract | 0 | 0 | 0 |
| `--stats` | file rewritten once a second with model counters | | | |

The profiles follow Samsung datasheets: PM883 960 GB (`sata-plp`), PM9A3
U.2 1.92 TB (`nvme-plp`, "No VWC present"), 870 EVO 1 TB
(`sata-consumer`). Latencies and IOPS are the datasheet's; steady-state
random write sets `waf` (for the 870 EVO no full-drive figure is
published, 12K IOPS is an estimate); `flush_us` is fitted to published
4K write + fsync measurements at QD1 (Ceph community SSD lists: PM883
15.5K/s, PM9A3 70K/s, 870 EVO 248–311/s). Buffer sizes are assumptions.

Calibration (`bench/calibrate_ssd.sh 30`, KVM guest with guest halt
polling, floor measured 15.8–16.4 µs, 4 GiB device):

| test | `sata-plp` | `nvme-plp` | `sata-consumer` | reference (spec / measured) |
|---|---|---|---|---|
| 4K random read QD1 | 121 µs | 82 µs | 79 µs | 120 / 80 / 77 µs |
| 4K random read QD32 | 96K | 197K (QD128 212K) | 96K | 98K / 850K / 98K |
| 128K sequential read QD32 | 553 MB/s | 4086 MB/s | 553 MB/s | 550 / 6800 / 560 |
| 128K sequential write QD32 | 553 MB/s | 2535 MB/s | 553 MB/s | 520 / 2700 / 530 |
| 4K random write QD1 | 42 µs | 23 µs | 11.8K IOPS | 40 / 30 (14.3 measured) µs / – |
| 4K random write QD32, steady | 24.6K | 124K | 11.7K | 25K / 130K / ~12K |
| 4K random write + fsync QD1 | 14.4–14.6K/s | 32.8–34K/s | 260–264/s | 15.5K / 70K / 248–311 |
| 4K read QD1 next to a write + fsync job | 4.8K, p99 0.77 ms | 7.0K | 519, p50 3.6 ms | |

NVMe numbers above ~200K IOPS or ~4 GB/s are the limit of one server
thread, not the model. A request can't complete faster than the floor
(16 µs here, ~35 µs without halt polling): the PM9A3 acknowledges a
synced write in 14 µs, the model at ~22 µs.

How these numbers were checked, what else is tested (model tests, data
integrity, delivered timing, real drives), and where the models stop
being valid: [docs/VALIDATION.md](docs/VALIDATION.md).

## Build and run

Needs a kernel with `ublk_drv` (6.0+; tested on 6.17) and liburing headers.

```bash
make
make check      # model tests on a virtual clock; no root, no ublk
sudo modprobe ublk_drv
# backing store: any block device or file that takes O_DIRECT I/O
sudo ./kublk add -t hdd -q 1 -d 32 --profile hgst-7k8 /dev/<ram-backed-dev>
sudo ./kublk add -t ssd -q 1 -d 32 --profile sata-plp --floor_us <floor> /dev/<ram-backed-dev2>
sudo ./kublk list
sudo ./kublk del -n <id>
```

The models are single-threaded: use `-q 1` (one actuator, one model
thread) and queue depth 32 to match SATA NCQ, 128 for NVMe.

`bench/calibrate.sh` (hdd) and `bench/calibrate_ssd.sh` (ssd; measures
the host's floor first) run fio micro-benchmarks (random/sequential reads
and writes, cached and synced writes, readers next to flushing and
caching writers) on a fresh device backed by a 4 GiB null_blk, print one
line per job, and with a stock profile check the results against
`bench/expect/`. `bench/integrity.sh` verifies that data reads back
intact on every target and cache mode.

Documentation:

- [docs/GUIDE.md](docs/GUIDE.md): step-by-step setup, using it under
  other software, the stats file, fitting a model to another drive;
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how it works, the model
  interface, invariants, adding a target;
- [docs/VALIDATION.md](docs/VALIDATION.md): the evidence and its limits;
- [CHANGELOG.md](CHANGELOG.md): model changes and which results they
  affect.

## Layout

| file | origin |
|---|---|
| `kublk.c`, `kublk.h`, `utils.h`, `ublk_dep.h`, `common.c`, `null.c`, `file_backed.c`, `fault_inject.c`, `stripe.c` | linux v6.17 selftests, small changes marked in git history |
| `include/linux/ublk_cmd.h` | linux v6.17 uapi, overrides older distro headers |
| `hdd.c`, `ssd.c`, `model_kublk.[ch]`, `model.h`, `hdd_model.[ch]`, `ssd_model.[ch]`, `tests/`, `bench/`, `docs/` | this project |

`stripe.c` is not built: it needs io_uring opcodes newer than Ubuntu
24.04's liburing 2.5 headers.

## License

GPL-2.0 (kublk selftests and this project's files); `kublk.c` itself is
MIT.
