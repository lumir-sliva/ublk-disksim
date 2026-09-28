# Changelog

Model changes change results. Each entry says what moved, so numbers can
be traced to the version that produced them; say which commit you used
when you report one.

## Unreleased

- **ssd write history** (results move only with `--history rnd` or on
  data your workload writes randomly and then reads in large pieces):
  the model remembers per `page_kb` chunk whether its data was written
  in one piece or scattered by random writes; a read of a scattered
  chunk is one page read per 4K. Default `--history seq` keeps every
  calibrated number. `ssd_model_new()` takes the device size.
- **ssd sequential writes pay `waf` by the scattered share of the
  drive** (results move for sequential writes after random writes):
  garbage collection takes its victims from the whole drive, so on a
  drive whose data random writes have scattered, sequential writes pay
  too (the real Micron after random preconditioning: ~300 MB/s).
- **ssd `--gc_mbps` / `--gc_pool_mb` / `--gc_idle_s`: garbage
  collection works ahead while idle** (off by default, no result moves):
  after `--gc_idle_s` of idle it prepares erased space, and writes after
  a pause use it at one program unit per page. Fitted to the Micron
  7300 PRO's bursts after idle (docs/VALIDATION.md): `--gc_pool_mb
  41000 --gc_mbps 400 --gc_idle_s 58`.
- Write stream detection continues the longest stream when several end
  at the same address.
- **`micron-7300` `cmd_us 0.56`** (results move above ~300K reads/s):
  4K random reads top out at 519K IOPS (datasheet 520K at QD512; the
  real drive 534K at QD256) instead of rising to ~700K. Every command,
  read or write, is 0.56 µs longer, and 128K sequential reads top out
  at ~2960 instead of 3000 MB/s.
- **`micron-7300` changes** (results move): TLC page types (`tr_us 25`,
  `tr_step_us 26`, same mean read), 64 KiB programs of 2.7 ms (`page_kb
  64`, `tprog_us 2624`, same program bandwidth) and `susp_us 20`, as
  measured on the drive. Calibration rows unchanged within noise; reads
  next to writers now wait ~20 µs mostly and a whole program rarely.
- **ssd `--tr_step_us X`: TLC page types.** Each 4K reads in `tr_us`,
  `tr_us + X` or `tr_us + 3X` (1, 2, 4 read levels), fixed per address,
  a third each. **Program suspend waits for the page to reach the
  die:** with `--susp_us`, a read arriving while the program's page is
  still crossing the channel (and the reads behind it) wait for the
  whole program; `--susp_us` results from the previous change move.
  0, the default of both, keeps the old behaviour.
- **ssd `--susp_us X`: program suspend.** A read that finds its die
  programming waits `X` µs (the program's next suspend point) instead
  of the rest of the program; the program resumes after the read. 0,
  the default, keeps the old behaviour (no profile used it then). New
  stats field `read_suspends`.
- **ssd profile `micron-7300`** (Micron 7300 PRO 3.84 TB, NVMe, PLP, no
  volatile cache), from the datasheet; no existing profile changes.
  `calibrate_ssd.sh REAL=<device>` runs its jobs on a real drive against
  the same expectations. `make LDFLAGS=-static` for older distributions.
  `make check` now catches cache space freed when a write-back starts
  instead of when it ends.
- **Calibration latencies are submission to completion.** The scripts
  run fio with `--lat_percentiles=1` and save json+ (full histograms);
  `lat_p50_us`/`lat_p99_us` in `results.tsv` were completion-only
  before, which left out ~17 µs of submission through ublk against ~2 µs
  on a real NVMe drive. fsync times that fio reports as < 1 µs (fio 3.28
  with libaio, fio on Windows) are derived from the write + fsync cycle.
  `calibrate.sh` adds QD 2–16 random reads. `REAL=` runs start 1 MiB
  into the device. Figures (`docs/img/`, `bench/figures.py`): scorecard,
  latency percentiles against real drives, queue depth, flush stall, and
  the tests at a glance: planted bugs, lateness, integrity.
- `bench/integrity.sh` also checks `barracuda-2t` and `micron-7300`:
  56 of 56 pass.
- **hdd `--wb_window N`:** write-back picks among the N writes that
  arrived first instead of the whole cache (0, the default, keeps the old
  behaviour). **Profile `barracuda-2t`**, fitted to a real Seagate
  ST2000DM006: slower seeks, `ncq` 4, `wb_window` 8, a 1 MiB cache for
  random writes. `hgst-7k8` results unchanged (same calibration within
  noise). calibrate.sh gains flushes after 8 and 64 scattered writes;
  `hgst-7k8`: 73 ms and 335 ms, `barracuda-2t`: 103 ms and 656 ms (the
  real drive: ~100 ms and ~600 ms, through NTFS).
- **Model cores separated from the server.** `hdd_model.c` and
  `ssd_model.c` see time, completions and wake-ups only through
  `model.h`; `model_kublk.c` is the shared kublk glue. Calibration
  unchanged within run-to-run noise, except `nvme-plp` steady random
  writes, +5% (less work per request in the server thread, closer to
  what the model allows).
- **Completion lateness** (actual − scheduled, at the completion timer)
  in the stats file: `late_us_p50/p99/p999/max`.
- **hdd write-back choice** keeps the dirty set sorted and searches
  outward from the head, stopping once the seek alone can't win. It is
  the same choice as the full scan (except exact ties), and cheap enough
  that a full cache of scattered 4K writes (~16K extents) no longer makes
  the server thread CPU-bound. Before, completions in that state fired
  late: p99 1.2 ms, max 14 ms. Results with a full cache of random
  writes are affected.
- **`make check`:** the models on a virtual clock. Scenario tests against
  documented costs, randomized tests with invariants.
- **`bench/integrity.sh`:** fio verify on every target and cache mode.
- **Pass/fail calibration:** the calibrate scripts write `results.tsv`
  and check it against `bench/expect/<profile>.tsv`.

## 97f26ef, 2026-09-26

- hdd: write-back moves at most one track per operation and frees space
  when it ends. Once the cache is 3/4 full it alternates one-for-one with
  queued requests, instead of waiting for an idle actuator. New
  `--max_wait_ms` (500): a request passed over that long goes next.
- Before this, cached writes stopped behind a steady read load and the
  next flush paid for all of them, and a reader next to a sequential
  writer waited ~230 ms. Results that mix reads with cached writes, or
  run write-through with a sequential writer, differ from earlier ones.

## fc373ad, 2026-09-26

- ssd: a flush with nothing written since the last one is free; SATA
  refuses a queue depth above 32; flush time is measured from arrival.

## 665c9db, 2026-09-26

- New target `ssd` (SATA or NVMe) with profiles `sata-plp`, `nvme-plp`,
  `sata-consumer`; `bench/calibrate_ssd.sh`.

## 1d92300, 2026-09-26

- `docs/GUIDE.md`.

## 95f09c0, 2026-09-26

- hdd: the actuator keeps its own timeline. A write-through write that
  continues where the head is but arrives after the previous one
  finished waits a revolution (1M sequential writes at QD1 with the cache
  off: 205 → 78 MB/s).

## b8f32fb, 2026-09-26

- New target `hdd`, profile `hgst-7k8`; `bench/calibrate.sh`.

## 774284e

- kublk from linux v6.17 (`tools/testing/selftests/ublk`).
