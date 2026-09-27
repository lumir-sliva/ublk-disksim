#!/usr/bin/env python3
# The figures in README and docs/, from calibration runs and fio output.
#
# Collect: turn run outputs into small tables in docs/img/data/ (rows are
# appended), so the figures can be redrawn without the raw runs. `lat`
# needs fio's json+ output with --lat_percentiles=1 (the histogram of
# submission-to-completion latency; without it, completion only).
#   figures.py lat   <table> <panel> <label> <fio json+> <job> <read|write>
#   figures.py qd    <table> <label> <fio json>...
#   figures.py score <table> <group> <results.tsv> <expect.tsv>
# Draw every figure whose table exists (needs matplotlib):
#   figures.py draw [data dir=docs/img/data] [image dir=docs/img]
import json
import math
import os
import sys

PCTS = (1, 5, 10, 25, 50, 75, 90, 95, 99, 99.5, 99.9, 99.95, 99.99)


def job_of(path, name=None):
    jobs = json.load(open(path))["jobs"]
    if not name:    # "" or None: the only (first) job
        return jobs[0]
    for j in jobs:
        if j["jobname"] == name:
            return j
    sys.exit(f"{path}: no job {name}")


def append(table, rows):
    with open(table, "a") as out:
        for r in rows:
            out.write("\t".join(str(x) for x in r) + "\n")
    print(f"{table}: +{len(rows)} rows")


def cmd_lat(table, panel, label, path, job, rw):
    s = job_of(path, job)[rw]
    bins = s["lat_ns"].get("bins") or s["clat_ns"].get("bins")
    if not bins:
        sys.exit(f"{path}: no latency bins (run fio with --output-format=json+)")
    pts = sorted((int(k), v) for k, v in bins.items())
    n = sum(v for _, v in pts)
    rows = [(panel, label, "n", n), (panel, label, "iops", f"{s['iops']:.1f}"),
            (panel, label, "mean", f"{s['lat_ns']['mean'] / 1e3:.4g}")]
    for q in PCTS:
        cum = 0
        for ns, v in pts:
            cum += v
            if cum >= q / 100 * n:
                rows.append((panel, label, f"p{q:g}", f"{ns / 1e3:.4g}"))
                break
    append(table, rows)


def cmd_qd(table, label, *paths):
    rows = []
    for p in paths:
        j = job_of(p)
        rw = "read" if j["read"]["io_bytes"] else "write"
        rows.append((label, int(j["job options"]["iodepth"]), f"{j[rw]['iops']:.1f}"))
    append(table, sorted(rows, key=lambda r: r[1]))


def cmd_score(table, group, results, expect):
    got = {}
    for line in open(results):
        f = line.rstrip("\n").split("\t")
        if len(f) == 5:
            got[tuple(f[:4])] = f[4]
    rows = []
    for line in open(expect):
        if not line.strip() or line.startswith("#"):
            continue
        label, job, rw, metric, kind, value, tol, _ = line.rstrip("\n").split("\t")
        if label == "stats" or kind == "=":
            continue    # host checks and exact counts: not a deviation
        g = got.get((label, job, rw, metric))
        if g is not None:
            rows.append((group, label, job, rw, metric, kind, value, tol, g))
    append(table, rows)


# ---- drawing -------------------------------------------------------------

INK, SOFT, FAINT = "#1d2327", "#5f6b73", "#e6e9eb"
REAL, MODEL, MODEL2 = "#d9480f", "#1c6dd0", "#74a9e8"
OK, BAD = "#2b8a3e", "#c92a2a"

JOBS = {
    "randread-qd1": "4K random read, QD1", "randread-qd32": "4K random read, QD32",
    "seqread-1m": "1M sequential read", "seqread-128k": "128K sequential read",
    "seqwrite-1m-qd1": "1M sequential write, QD1",
    "seqwrite-1m-qd4": "1M sequential write, QD4",
    "seqwrite-128k": "128K sequential write", "randwrite-qd1": "4K random write, QD1",
    "randwrite-qd32": "4K random write, QD32", "randwrite-fsync": "4K write + fsync",
    "randwrite-fsync8": "flush after 8 writes", "randwrite-fsync64": "flush after 64 writes",
    "seqwrite-fsync": "4K append + fsync",
}
# jobs with two fio jobs: what the checked one is doing
MIXED = {
    ("blocking", "reader"): "reader next to a write + fsync job",
    ("blocking", "writer"): "writer (write + fsync) next to a reader",
    ("read-vs-cache", "writer"): "cached writer next to QD4 readers",
    ("read-vs-cache", "reader"): "QD4 readers next to a cached writer",
    ("read-vs-seq", "reader"): "reader next to a sequential writer",
    ("full-fsync", "fsyncer"): "write + fsync next to a bulk writer",
}
METRIC = {"lat_mean_us": "", "lat_p50_us": "median", "lat_p99_us": "p99",
          "fsync_mean_us": "fsync", "iops": "", "mbps": "", "flush_ms": ""}


def row_name(label, job, metric):
    name = MIXED.get((label, job)) or JOBS.get(job) or JOBS.get(label) or job
    m = METRIC.get(metric, metric)
    return f"{name}, {m}" if m and m not in name else name


def setup():
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.rcParams.update({
        "svg.fonttype": "none", "svg.hashsalt": "ublk-disksim",
        "font.family": "sans-serif",
        "font.sans-serif": ["Helvetica Neue", "Helvetica", "Arial", "DejaVu Sans"],
        "font.size": 11, "text.color": INK, "axes.labelcolor": SOFT,
        "xtick.color": SOFT, "ytick.color": SOFT, "axes.edgecolor": "#c3c9cd",
        "axes.spines.top": False, "axes.spines.right": False,
        "axes.spines.left": False, "axes.grid": True, "grid.color": FAINT,
        "grid.linewidth": 1, "xtick.major.size": 0, "ytick.major.size": 0,
        "figure.facecolor": "white", "axes.facecolor": "white",
        "legend.frameon": False,
    })
    return plt


def headline(fig, title, sub):
    """Title and subtitle at fixed distances (inches) from the top."""
    H = fig.get_figheight()
    fig.text(0.012, 1 - 0.14 / H, title, fontsize=15, fontweight="bold",
             va="top", ha="left")
    fig.text(0.012, 1 - 0.52 / H, sub, fontsize=10.5, color=SOFT, va="top",
             ha="left", linespacing=1.4)


def save(plt, fig, path):
    fig.savefig(path, metadata={"Date": None}, facecolor="white")
    plt.close(fig)
    print(f"wrote {path}")


def rows_of(path):
    return [l.rstrip("\n").split("\t") for l in open(path) if l.strip()]


def fmt(metric, v):
    if metric.endswith("_us"):
        return f"{v / 1e3:.3g} ms" if v >= 1000 else f"{v:.3g} µs"
    if metric == "iops":
        return f"{v / 1e3:.3g}K IOPS" if v >= 1000 else f"{v:.3g} IOPS"
    if metric == "mbps":
        return f"{v:.0f} MB/s"
    if metric == "flush_ms":
        return f"{v:.3g} ms"
    return f"{v:.3g}"


def lat_ms(us):
    return f"{us / 1e3:.3g} ms" if us >= 1000 else f"{us:.3g} µs"


def draw_score(plt, table, out):
    from matplotlib.transforms import blended_transform_factory as blend
    rows = rows_of(table)
    # models first, then the real drive judged by its own datasheet
    is_real = lambda g: "model" not in g
    groups = sorted(dict.fromkeys(r[0] for r in rows), key=is_real)
    lim = 60                                  # % deviation shown
    h = 0.31 * len(rows) + 0.72 * len(groups) + 1.9
    fig = plt.figure(figsize=(10.5, h))
    ax = fig.add_axes([0.345, 0.62 / h, 0.34, 1 - 1.95 / h])
    lab = blend(fig.transFigure, ax.transData)
    y, npass, nmodel = 0, 0, 0
    for g in groups:
        real = is_real(g)
        y -= 0.45
        ax.plot([0.012, 0.988], [y + 0.55, y + 0.55], transform=lab, color=FAINT,
                lw=1, clip_on=False)
        ax.text(0.012, y, g, transform=lab, fontweight="bold", fontsize=11,
                va="center", color=REAL if real else INK)
        y -= 0.95
        for _, label, job, rw, metric, kind, value, tol, got in (r for r in rows if r[0] == g):
            value, tol, got = float(value), float(tol), float(got)
            dev = (got / value - 1) * 100
            if kind == "~":
                band, ok = (-tol * 100, tol * 100), abs(dev) <= tol * 100
            elif kind == "<=":
                band, ok = (-lim, 0), dev <= 0
            else:
                band, ok = (0, lim), dev >= 0
            if not real:
                nmodel += 1
                npass += ok
            c = OK if ok else BAD
            ax.add_patch(plt.Rectangle((band[0], y - 0.34), band[1] - band[0], 0.68,
                                       color="#e6f2e8", lw=0, zorder=0))
            clipped = abs(dev) > lim
            shown = max(-lim, min(lim, dev))
            ax.plot((0, shown), (y, y), color=c, lw=2.2, zorder=2)
            ax.plot(shown, y, marker=(">" if dev > 0 else "<") if clipped else "o",
                    color=c, ms=8, zorder=3)
            txt = "±0%" if round(dev) == 0 else f"{dev:+.0f}%".replace("-", "−")
            if clipped:     # label above the arrow, the line runs under it
                ax.text(shown, y + 0.36, txt, color=c, fontsize=9.5, va="bottom",
                        ha="right" if dev > 0 else "left", fontweight="bold")
            else:
                right = dev >= 0
                ax.text(shown + (3.5 if right else -3.5), y, txt, color=c,
                        fontsize=9.5, va="center", ha="left" if right else "right",
                        fontweight="bold", zorder=4)
            ax.text(0.03, y, row_name(label, job, metric), transform=lab,
                    va="center", fontsize=10)
            bound = {"~": "", "<=": "≤ ", ">=": "≥ "}[kind]
            ax.text(0.715, y, fmt(metric, got), transform=lab, va="center",
                    fontsize=10, fontweight="bold", color=c)
            ax.text(0.845, y, f"{bound}{fmt(metric, value)}", transform=lab,
                    va="center", fontsize=10, color=SOFT)
            y -= 1
    top = blend(fig.transFigure, ax.transAxes)
    for x, t in ((0.715, "measured"), (0.845, "reference")):
        ax.text(x, 1.01, t, transform=top, fontsize=9, color=SOFT, va="bottom")
    ax.set_xlim(-lim, lim)
    ax.set_ylim(y + 0.4, 0)
    ax.set_yticks([])
    ax.grid(axis="y", visible=False)
    ax.spines["bottom"].set_visible(False)
    ax.axvline(0, color=INK, lw=1)
    ax.set_xticks([-50, -25, 0, 25, 50], ["−50%", "−25%", "0", "+25%", "+50%"])
    ax.set_xlabel("deviation from the reference; green band = tolerance",
                  fontsize=9.5)
    headline(fig, f"Models: {npass} of {nmodel} calibration checks within tolerance",
             "fio on each model against its reference (bench/expect/): a "
             "datasheet, or measurements of the real drive it imitates.\n"
             "The misses are the test host's limits: one server thread reaches "
             "~85K IOPS there, and its per-request overhead\nexceeds a PLP "
             "SSD's 40 µs write. Last: the real Micron 7300 PRO against its "
             "own datasheet.")
    save(plt, fig, out)


def pct_series(rows, panel, label):
    d = {r[2]: float(r[3]) for r in rows if r[0] == panel and r[1] == label}
    n = d.get("n", 0)
    pts = [(q, d[f"p{q:g}"]) for q in PCTS
           if f"p{q:g}" in d and q >= 50 and (1 - q / 100) * n >= 10]
    return pts, d


def nines(q):
    return -math.log10(1 - q / 100)


def log_ticks(lo, hi, fmt_fn):
    """At most ~5 readable ticks on a log axis between lo and hi."""
    for steps in ((1,), (1, 3), (1, 2, 5), (1, 1.5, 2, 3, 5, 7)):
        ticks = [m * 10 ** e for e in range(-1, 8) for m in steps
                 if lo <= m * 10 ** e <= hi]
        if len(ticks) >= 3:
            break
    return ticks, [fmt_fn(t) for t in ticks]


def lat_axis(ax, vals):
    from matplotlib.ticker import FixedLocator, NullLocator
    ax.set_yscale("log")
    lo, hi = min(vals) / 1.25, max(vals) * 1.25
    ax.set_ylim(lo, hi)
    ticks, labels = log_ticks(lo, hi, lat_ms)
    ax.yaxis.set_major_locator(FixedLocator(ticks))
    ax.yaxis.set_minor_locator(NullLocator())
    ax.set_yticklabels(labels)


def draw_lat(plt, table, out, title, sub, panels_meta):
    """Latency percentiles per panel, model vs real drive."""
    from matplotlib.lines import Line2D
    rows = rows_of(table)
    panels = list(dict.fromkeys(r[0] for r in rows))
    ncol = 2 if len(panels) > 1 else 1
    nrow = math.ceil(len(panels) / ncol)
    fig = plt.figure(figsize=(10, 1.75 + 3.2 * nrow))
    H = fig.get_figheight()
    gs = fig.add_gridspec(nrow, ncol, left=0.085, right=0.975, bottom=0.5 / H,
                          top=1 - 2.0 / H, hspace=0.75, wspace=0.22)
    for i, p in enumerate(panels):
        ax = fig.add_subplot(gs[i // ncol, i % ncol])
        allv, xmax, model = [], 0, ""
        for lab in dict.fromkeys(r[1] for r in rows if r[0] == p):
            pts, _ = pct_series(rows, p, lab)
            real = "real" in lab
            if not real:
                model = lab
            xs, ys = [nines(q) for q, _ in pts], [v for _, v in pts]
            ax.plot(xs, ys, color=REAL if real else MODEL, lw=2.4, marker="o",
                    ms=3.5, zorder=3, ls=(0, (4, 1.6)) if real else "-")
            allv += ys
            xmax = max(xmax, xs[-1])
        lat_axis(ax, allv)
        qs = (50, 90, 99, 99.9, 99.99)
        ax.set_xticks([nines(q) for q in qs],
                      ["median", "p90", "p99", "p99.9", "p99.99"])
        ax.set_xlim(nines(50) - 0.12, xmax + 0.12)
        ax.grid(axis="x", visible=False)
        head, note = panels_meta.get(p, (p, ""))
        ax.set_title(head, fontsize=11.5, fontweight="bold", loc="left", pad=22)
        ax.text(0, 1.035, note, transform=ax.transAxes, fontsize=9.5,
                color=SOFT, va="bottom")
        ax.text(1, 1.035, model, transform=ax.transAxes, fontsize=9.5,
                color=MODEL, va="bottom", ha="right", fontweight="bold")
    fig.legend(handles=[Line2D([], [], color=REAL, lw=2.4, ls=(0, (4, 1.6))),
                        Line2D([], [], color=MODEL, lw=2.4)],
               labels=["real drive", "model"], loc="upper right",
               bbox_to_anchor=(0.975, 1 - 1.2 / H), ncol=2, fontsize=10.5,
               handlelength=2.6)
    headline(fig, title, sub)
    save(plt, fig, out)


def draw_qd(plt, table, out):
    rows = rows_of(table)
    fig = plt.figure(figsize=(10, 5.2))
    ax = fig.add_axes([0.08, 0.12, 0.62, 0.64])
    model_c = iter([MODEL, MODEL2])
    ends = []
    for lab in dict.fromkeys(r[0] for r in rows):
        pts = [(int(r[1]), float(r[2])) for r in rows if r[0] == lab]
        real = "real" in lab
        c = REAL if real else next(model_c)
        ax.plot([q for q, _ in pts], [v for _, v in pts], color=c, lw=2.6,
                marker="o", ms=6, ls=(0, (5, 1.5)) if real else "-")
        q1, q32 = pts[0][1], pts[-1][1]
        ends.append([q32, q32, f"{lab.replace(' (real)', ' (real drive)')}\n"
                     f"{q1:.0f} → {q32:.0f} IOPS, {q32 / q1:.1f}×", c])
    # labels at the line ends, at least 12% of the axis apart
    top = max(e[0] for e in ends) * 1.15
    ends.sort(key=lambda e: e[0])
    for a, b in zip(ends, ends[1:]):
        b[1] = max(b[1], a[1] + 0.12 * top)
    for y0, y, text, c in ends:
        ax.annotate(text, (32, y0), xytext=(40, y), textcoords="data",
                    color=c, fontsize=10, fontweight="bold", va="center",
                    annotation_clip=False, linespacing=1.3)
    ax.set_ylim(0, max(top, ends[-1][1] * 1.05))
    ax.set_xscale("log", base=2)
    ax.set_xticks([1, 2, 4, 8, 16, 32], ["1", "2", "4", "8", "16", "32"])
    ax.minorticks_off()
    ax.set_xlabel("requests in flight (4K random reads over the whole disk)")
    ax.set_ylabel("IOPS")
    ax.grid(axis="x", visible=False)
    headline(fig, "Queueing pays off on an enterprise disk, barely on a consumer one",
             "A drive that reorders its queue (NCQ) serves the nearest request "
             "next. hgst-7k8 assumes it does that well;\nbarracuda-2t is fitted "
             "to a real ST2000DM006 (dashed), which gains little from a deep queue.")
    save(plt, fig, out)


def draw_flush(plt, table, out):
    """Reads/s and p99 read latency, alone and next to a writer that fsyncs."""
    rows = rows_of(table)
    devs = list(dict.fromkeys(r[0] for r in rows))
    get = lambda d, lab, s: float(next(r[3] for r in rows
                                       if r[0] == d and r[1] == lab and r[2] == s))
    fig = plt.figure(figsize=(10, 2.0 + 0.95 * len(devs)))
    H = fig.get_figheight()
    ax1 = fig.add_axes([0.25, 0.75 / H, 0.36, 1 - 2.4 / H])
    ax2 = fig.add_axes([0.69, 0.75 / H, 0.27, 1 - 2.4 / H])
    for ax, t in ((ax1, "4K reads per second"), (ax2, "p99 read latency")):
        ax.text(0, 1.04, t, transform=ax.transAxes, fontsize=11,
                fontweight="bold", va="bottom")
    for i, d in enumerate(devs):
        y = -i
        real = "real" in d
        c = REAL if real else MODEL
        alone, busy = get(d, "alone", "iops"), get(d, "next to fsync writer", "iops")
        ax1.barh(y + 0.19, alone, 0.36, color=c, alpha=0.3, lw=0)
        ax1.barh(y - 0.19, busy, 0.36, color=c, lw=0)
        ax1.text(busy, y - 0.19, f"  {busy:,.0f}/s  {(busy / alone - 1) * 100:+.0f}%"
                 .replace("-", "−"), va="center", fontsize=9.5, color=c,
                 fontweight="bold")
        ax1.text(alone, y + 0.19, f"  {alone:,.0f}/s alone", va="center",
                 fontsize=9.5, color=SOFT)
        pa, pb = get(d, "alone", "p99"), get(d, "next to fsync writer", "p99")
        ax2.plot((pa, pb), (y, y), color=c, lw=2, alpha=0.5)
        ax2.plot(pa, y, "o", color=c, alpha=0.35, ms=9)
        ax2.plot(pb, y, "o", color=c, ms=9)
        ax2.text(pb, y + 0.26, lat_ms(pb), ha="center", fontsize=9.5, color=c,
                 fontweight="bold")
        ax2.text(pa, y + 0.26, lat_ms(pa), ha="center", fontsize=9, color=SOFT)
        ax1.text(-0.04, y, d.replace(" (real", "\n(real").replace(" (", "\n("),
                 transform=ax1.get_yaxis_transform(), ha="right", va="center",
                 fontsize=10.5, fontweight="bold", color=c, linespacing=1.3)
    for ax in (ax1, ax2):
        ax.set_yticks([])
        ax.set_ylim(-len(devs) + 0.45, 0.62)
        ax.grid(axis="y", visible=False)
    ax1.set_xlim(0, max(get(d, "alone", "iops") for d in devs) * 1.45)
    from matplotlib.ticker import MultipleLocator
    ax1.xaxis.set_major_locator(MultipleLocator(5000 if ax1.get_xlim()[1] > 12000 else 2000))
    ax1.xaxis.set_major_formatter(lambda v, _: f"{v / 1e3:.0f}K")
    ax1.set_xlabel("QD1 reader; light bar: alone, dark: next to the writer",
                   fontsize=9.5)
    ax2.set_xscale("log")
    lo = min(get(d, lab, "p99") for d in devs for lab in ("alone", "next to fsync writer"))
    hi = max(get(d, lab, "p99") for d in devs for lab in ("alone", "next to fsync writer"))
    ax2.set_xlim(lo / 1.8, hi * 1.8)
    from matplotlib.ticker import FixedLocator, NullLocator
    ticks = [t for t in (1, 10, 100, 1e3, 1e4, 1e5, 1e6) if lo / 1.8 <= t <= hi * 1.8]
    ax2.xaxis.set_major_locator(FixedLocator(ticks))
    ax2.xaxis.set_minor_locator(NullLocator())
    ax2.set_xticklabels([lat_ms(t) for t in ticks])
    ax2.set_xlabel("faint: alone, solid: next to the writer", fontsize=9.5)
    headline(fig, "Without power-loss protection, every fsync stalls the reads",
             "A reader next to a writer that fsyncs every 4K write. Without PLP "
             "the drive must program its buffer before\nanswering the flush, "
             "and a SATA flush holds up everything queued behind it.")
    save(plt, fig, out)


PANELS = {
    "micron-read": ("Micron 7300 PRO (NVMe)", "4K random read, QD1"),
    "micron-mixed": ("Micron 7300 PRO (NVMe)", "4K read next to a writer that fsyncs"),
    "evo-read": ("Samsung 850 EVO (SATA)", "4K random read, QD1"),
    "hdd-read": ("Seagate ST2000DM006 (HDD)", "4K random read, QD1, whole disk"),
}


def cmd_draw(data="docs/img/data", img="docs/img"):
    plt = setup()
    os.makedirs(img, exist_ok=True)
    f = lambda n: os.path.join(data, n)
    if os.path.exists(f("score.tsv")):
        draw_score(plt, f("score.tsv"), os.path.join(img, "scorecard.svg"))
    if os.path.exists(f("latency.tsv")):
        draw_lat(plt, f("latency.tsv"), os.path.join(img, "latency.svg"),
                 "The models against real drives, request by request",
                 "Latency percentiles from fio, submission to completion. The "
                 "models were fitted to datasheets or to earlier\nmeasurements "
                 "of the same drive; none of these runs was used for fitting.",
                 PANELS)
    if os.path.exists(f("qd.tsv")):
        draw_qd(plt, f("qd.tsv"), os.path.join(img, "queue-depth.svg"))
    if os.path.exists(f("flush.tsv")):
        draw_flush(plt, f("flush.tsv"), os.path.join(img, "flush-reader.svg"))


if __name__ == "__main__":
    cmds = {"lat": cmd_lat, "qd": cmd_qd, "score": cmd_score, "draw": cmd_draw}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        sys.exit(open(__file__).read().split("\nimport")[0])
    cmds[sys.argv[1]](*sys.argv[2:])
