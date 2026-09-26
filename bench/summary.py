#!/usr/bin/env python3
# One line per fio job and direction for the calibration scripts, and the
# same numbers appended to a results file for bench/check.py.
#
# usage: summary.py <fio json> <label> <ms|us> <results.tsv>
#
# results.tsv lines: label <TAB> job <TAB> rw <TAB> metric <TAB> value, with
# metrics iops, mbps, lat_mean_us, lat_p50_us, lat_p99_us, fsync_mean_us.
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
            p = s["clat_ns"].get("percentile", {})
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
            print(f"{label:<16} {name:<16} fsync n {sy['N']}  "
                  f"mean {sy['mean'] / scale:8.2f} {unit}")
            out.write(f"{label}\t{name}\tsync\tfsync_mean_us\t{sy['mean'] / 1e3:.3f}\n")
