# Figures

Drawn by `bench/figures.py draw` from the tables in `data/`; each table
is collected from calibration runs and fio output with the other
`figures.py` subcommands, so a figure can be redrawn without the raw
runs and checked against the numbers it shows.

| figure | table | from |
|---|---|---|
| `scorecard.svg` | `data/score.tsv` | `figures.py score`: `results.tsv` of each calibration run against its `bench/expect/<profile>.tsv` |
| `latency.svg` | `data/latency.tsv` | `figures.py lat`: latency percentiles of one fio job (`--output-format=json+ --lat_percentiles=1`) |
| `queue-depth.svg` | `data/qd.tsv` | `figures.py qd`: IOPS of the 4K random read jobs at QD 1–32 |
| `flush-reader.svg` | `data/flush.tsv` | `figures.py lat`: the QD1 read job alone and the reader of the "reader next to a writer that fsyncs" job |
| `integrity.svg` | `data/integrity.tsv` | `figures.py integ`: the PASS/FAIL lines of `bench/integrity.sh` and of its `NEGATIVE=1` run |
| `validation-overview.svg`, `mutation.svg`, `lateness.svg` | `data/tests.json` | the recorded results of `make check`, the planted-bug runs and the lateness table in VALIDATION.md, entered by hand; the overview also counts `data/integrity.tsv` |

Where the runs came from:

- Models and the Micron 7300 PRO: `bench/calibrate.sh 30` and
  `bench/calibrate_ssd.sh 30 --profile <p>` on one KVM guest (16 vCPU
  AMD EPYC, Ubuntu 22.04, linux 6.8, guest halt polling on); the Micron
  7300 PRO 3.84 TB passed through to that guest and measured with
  `REAL=/dev/nvme0n1 bench/calibrate_ssd.sh 30 --profile micron-7300`,
  so drive and model ran on the same host.
- Samsung 850 EVO 250 GB and Seagate ST2000DM006: fio 3.43 on Windows
  (`windowsaio`, `--direct=1`, a 2 GiB / 4 GiB test file on NTFS; on
  the nearly full HDD the file is spread over the whole platter), the
  same jobs and options as the calibration scripts.

Redraw after new runs:

```bash
python3 bench/figures.py score docs/img/data/score.tsv "<group>" <run>/results.tsv bench/expect/<profile>.tsv
python3 bench/figures.py lat docs/img/data/latency.tsv <panel> "<label>" <run>/randread-qd1.json randread-qd1 read
python3 bench/figures.py qd docs/img/data/qd.tsv "<label>" <run>/randread-qd{1,2,4,8,16,32}.json
python3 bench/figures.py draw          # needs matplotlib
```

Tables are appended to; start from an empty file to replace a series. A
label containing "real" is drawn as a real drive (orange, dashed).
