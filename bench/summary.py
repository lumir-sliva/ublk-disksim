#!/usr/bin/env python3
# One line per fio job and direction for the calibration scripts, and the
# same numbers appended to a results file for bench/check.py.
#
# usage: summary.py <fio json> <label> <ms|us> <results.tsv>
#
# results.tsv lines: label <TAB> job <TAB> rw <TAB> metric <TAB> value, with
# metrics iops, mbps, lat_mean_us, lat_p50_us, lat_p99_us, fsync_mean_us.
# Latencies are submission to completion (fio's lat); percentiles need
# fio --lat_percentiles=1, else they fall back to completion only (clat).
import json
import sys

path, label, unit, results = sys.argv[1:5]
scale = 1e6 if unit == "ms" else 1e3
d = json.load(open(path))
with open(results, "a") as out:
    for j in d["jobs"]:
        name = j["jobname"]
        for rw in ("read", "write"):
            s = j[rw]
            if not s["io_bytes"]:
                continue
            p = (s["lat_ns"].get("percentile") or
                 s["clat_ns"].get("percentile", {}))
            mean = s["lat_ns"]["mean"]
            p50, p99 = p.get("50.000000", 0), p.get("99.000000", 0)
            print(f"{label:<16} {name:<16} {rw:<5} iops {s['iops']:9.1f}  "
                  f"MB/s {s['bw_bytes'] / 1e6:7.1f}  lat mean {mean / scale:8.2f}  "
                  f"p50 {p50 / scale:8.2f}  p99 {p99 / scale:8.2f} {unit}")
            for metric, v in (("iops", s["iops"]), ("mbps", s["bw_bytes"] / 1e6),
                              ("lat_mean_us", mean / 1e3),
                              ("lat_p50_us", p50 / 1e3), ("lat_p99_us", p99 / 1e3)):
                out.write(f"{label}\t{name}\t{rw}\t{metric}\t{v:.3f}\n")
        sy = j.get("sync", {}).get("lat_ns", {})
        if sy.get("N"):
            mean = sy["mean"]
            w, n = j["write"], int(j["job options"].get("fsync", 0))
            if mean < 1000 and n and w["iops"]:
                # Some fio builds (3.28 with libaio; Windows) report ~0.3 us
                # for fsync, no real flush is that fast. At QD1 the job
                # alternates n writes and an fsync, so the fsync takes
                # what the writes leave of each cycle.
                mean = n * (1e9 / w["iops"] - w["lat_ns"]["mean"])
                how = "from the write+fsync cycle; fio's own figure is unusable"
            else:
                how = ""
            print(f"{label:<16} {name:<16} fsync n {sy['N']}  "
                  f"mean {mean / scale:8.2f} {unit} {how}")
            out.write(f"{label}\t{name}\tsync\tfsync_mean_us\t{mean / 1e3:.3f}\n")
