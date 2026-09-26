#!/usr/bin/env python3
# Compare a calibration run with the expected values of its profile.
#
# usage: check.py <results.tsv> <expect.tsv> [model.stats]
#
# expect.tsv: label  job  rw  metric  kind  value  tolerance  source
#   kind "~": within tolerance (a fraction) of value; "<=" / ">=": a bound;
#   "=": exactly value. label "stats" takes the metric from the model's
#   stats file (e.g. late_us_p99). Lines starting with # are comments.
# Prints one line per expectation and exits 1 if any fails or is missing.
import sys


def load_results(path):
    r = {}
    for line in open(path):
        f = line.rstrip("\n").split("\t")
        if len(f) == 5:
            r[tuple(f[:4])] = float(f[4])
    return r


def load_stats(path):
    s = {}
    if path:
        for line in open(path):
            f = line.split()
            if len(f) == 2:
                s[f[0]] = float(f[1])
    return s


def main():
    results = load_results(sys.argv[1])
    stats = load_stats(sys.argv[3] if len(sys.argv) > 3 else None)
    bad = 0
    for line in open(sys.argv[2]):
        if not line.strip() or line.startswith("#"):
            continue
        label, job, rw, metric, kind, value, tol, source = \
            line.rstrip("\n").split("\t")
        value, tol = float(value), float(tol)
        got = stats.get(metric) if label == "stats" else \
            results.get((label, job, rw, metric))
        if got is None:
            ok, how = False, "missing"
        elif kind == "~":
            ok = abs(got - value) <= tol * value
            how = f"{got:.4g} vs {value:.4g} ±{tol:.0%}"
        elif kind == "<=":
            ok, how = got <= value, f"{got:.4g} <= {value:.4g}"
        elif kind == ">=":
            ok, how = got >= value, f"{got:.4g} >= {value:.4g}"
        else:
            ok, how = got == value, f"{got:.4g} = {value:.4g}"
        bad += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {label} {job} {rw} {metric}: "
              f"{how}  ({source})")
    print(f"check: {'all passed' if not bad else f'{bad} failed'}")
    sys.exit(1 if bad else 0)


main()
