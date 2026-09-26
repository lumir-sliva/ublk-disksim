# Changelog

Model changes change results. Each entry says what moved, so numbers can
be traced to the version that produced them; say which commit you used
when you report one.

## Unreleased

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
