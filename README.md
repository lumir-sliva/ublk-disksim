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

What it models (see the header of `hdd.c`):

- one actuator: requests are served one at a time,
  shortest-positioning-time-first among the oldest `ncq` waiting ones
  (NCQ with rotational position ordering);
- positioning = seek (sqrt curve over the distance, none within a track) +
  waiting for the sector to rotate under the head (sector angle from the
  LBA, platter phase from the clock), then media transfer; a request
  starting where the head is pays no positioning;
- volatile write cache: writes complete after the host transfer and are
  destaged when the actuator is idle; a full cache makes writes wait;
- FLUSH as SATA `FLUSH CACHE`: non-queued. Queued requests finish, the
  whole cache is destaged, and everything that arrives meanwhile, reads
  included, waits;
- `--cache_mb 0` = write-through (write cache off, or a controller that
  hides it): no volatile cache is advertised, so the kernel sends no
  flushes, and every write pays mechanical time.

Not modelled: zoned transfer rates (one rate, one track size), read cache
and read-ahead beyond "sequential costs nothing", firmware limits on dirty
data, destage idle timers.

Parameters (`kublk add -t hdd ... --<name> <value>`):

| option | meaning | `hgst-7k8` |
|---|---|---|
| `--profile` | named parameter set, applied first | |
| `--rpm` | spindle speed | 7200 |
| `--seek_min_ms` | track-to-track seek | 0.6 |
| `--seek_avg_ms` | average random seek | 8.0 |
| `--mbps` | media transfer rate | 205 |
| `--iface_mbps` | host link rate | 600 |
| `--iface_us` | per-command overhead | 30 |
| `--cache_mb` | volatile write cache, 0 = write-through | 64 |
| `--ncq` | requests considered for reordering | 32 |
| `--stroke` | fraction of the full stroke the device spans | 1.0 |
| `--seed` | platter phase at start | 0 |
| `--stats` | file rewritten once a second with model counters | |

`hgst-7k8` follows the HGST/WD Ultrastar 7K8 (HUS728T8TALE6L4, 8 TB SATA)
spec sheet: 8 ms average seek, 4.16 ms average latency, 205 MB/s internal
rate, 6 Gb/s SATA, 256 MB buffer. How much of the buffer caches writes
isn't published; 64 MiB is an assumption.

Calibration of `hgst-7k8` (`bench/calibrate.sh 30`, kernel 6.17, 4 GiB
device, so seeks span the full stroke):

| test | model | reference |
|---|---|---|
| 4K random read QD1 | 80 IOPS, 12.5 ms mean | 8.0 + 4.16 ms = 12.2 ms (spec) |
| 4K random read QD32 | 200 IOPS | ~200 for 7200 rpm SATA with NCQ (typical) |
| 1M sequential read QD1 | 185 MB/s | 205 MB/s internal, 255 MB/s outer (spec) |
| 4K random write + fsync | 80/s, flush 12.5 ms | 13–16 ms median per flush on these drives in production, at 0.4–5 flushes/s with ~4 larger writes per flush |
| flush of a full 64 MiB cache of random 4K writes | 16.7 s | unknown |
| 4K read QD1 next to a write+fsync QD1 job | 40 IOPS, 24.5 ms p50 | reads wait for the flush |
| write-through, 4K random write QD1 | 80 IOPS, 0 flushes at the device | |

## Build and run

Needs a kernel with `ublk_drv` (6.0+; tested on 6.17) and liburing headers.

```bash
make
sudo modprobe ublk_drv
# backing store: any block device or file that takes O_DIRECT I/O
sudo ./kublk add -t hdd -q 1 -d 32 --profile hgst-7k8 /dev/<ram-backed-dev>
sudo ./kublk list
sudo ./kublk del -n <id>
```

The model is single-threaded: use `-q 1` (one actuator) and queue depth 32
to match SATA NCQ.

`bench/calibrate.sh` runs fio micro-benchmarks (random/sequential reads,
cached and synced writes, a reader next to a flushing writer) on a fresh
device backed by a 4 GiB null_blk and prints one line per job.

## Layout

| file | origin |
|---|---|
| `kublk.c`, `kublk.h`, `utils.h`, `ublk_dep.h`, `common.c`, `null.c`, `file_backed.c`, `fault_inject.c`, `stripe.c` | linux v6.17 selftests, small changes marked in git history |
| `include/linux/ublk_cmd.h` | linux v6.17 uapi, overrides older distro headers |
| `hdd.c`, `bench/` | this project |

`stripe.c` is not built: it needs io_uring opcodes newer than Ubuntu
24.04's liburing 2.5 headers.

## License

GPL-2.0 (kublk selftests and `hdd.c`); `kublk.c` itself is MIT.
