#!/usr/bin/env bash
# Calibration micro-benchmarks for a kublk hdd device.
#
# usage: sudo bench/calibrate.sh [runtime_s=60] [extra kublk hdd options...]
#
# Creates a memory-backed null_blk (4 GiB, configfs name ublksim<ID>) as
# the backing store, starts `kublk add -t hdd` on it as /dev/ublkb<ID>,
# runs the fio jobs below one at a time and prints one summary line per
# job, then tears everything down. Results (fio json, model stats) go to
# $OUT.
set -euo pipefail

RT=${1:-60}; shift || true
ID=${ID:-10}
OUT=${OUT:-/tmp/ublk-disksim-cal-$(date +%s)}
HERE=$(cd "$(dirname "$0")/.." && pwd)
CFG=/sys/kernel/config/nullb/ublksim$ID
BACK=/dev/ublksim$ID    # null_blk configfs devices are named after the dir
DEV=/dev/ublkb$ID
mkdir -p "$OUT"

cleanup() {  # only what this run created
    [ -n "${ADDED:-}" ] && { "$HERE/kublk" del -n "$ID" >/dev/null 2>&1 || true; }
    if [ -d "$CFG" ]; then
        echo 0 > "$CFG/power"
        rmdir "$CFG"
    fi
}

modprobe null_blk nr_devices=0 2>/dev/null || true
modprobe ublk_drv
mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config
mkdir "$CFG"            # fails if another run uses this ID: nothing to undo
trap cleanup EXIT
for kv in size=4096 blocksize=512 memory_backed=1 irqmode=0 queue_mode=2; do
    echo "${kv#*=}" > "$CFG/${kv%%=*}"
done
echo 1 > "$CFG/power"

"$HERE/kublk" add -t hdd -n "$ID" -q 1 -d 32 --stats "$OUT/model.stats" \
    "$@" "$BACK" | tee "$OUT/kublk.txt"
ADDED=1
udevadm settle
echo "device $DEV: rotational $(cat /sys/block/ublkb$ID/queue/rotational)" \
     "write_cache '$(cat /sys/block/ublkb$ID/queue/write_cache)'"

# fill once so reads hit written data
fio --name=fill --filename="$DEV" --rw=write --bs=1M --size=1G --direct=1 \
    --ioengine=libaio --iodepth=4 --output=/dev/null

summary() {  # fio json, label
    python3 - "$1" "$2" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
for j in d["jobs"]:
    for rw in ("read", "write"):
        s = j[rw]
        if not s["io_bytes"]:
            continue
        c = s["clat_ns"]
        p = c.get("percentile", {})
        print(f'{sys.argv[2]:<16} {j["jobname"]:<16} {rw:<5} '
              f'iops {s["iops"]:8.1f}  MB/s {s["bw_bytes"]/1e6:7.1f}  '
              f'lat mean {c["mean"]/1e6:7.2f}  p50 {p.get("50.000000", 0)/1e6:7.2f}  '
              f'p99 {p.get("99.000000", 0)/1e6:7.2f} ms')
    sy = j.get("sync", {}).get("lat_ns", {})
    if sy.get("N"):
        print(f'{sys.argv[2]:<16} {j["jobname"]:<16} fsync n {sy["N"]}  '
              f'mean {sy["mean"]/1e6:7.2f} ms')
EOF
}

run() {  # name, fio args...
    local name=$1; shift
    fio --filename="$DEV" --direct=1 --ioengine=libaio \
        --time_based --runtime="$RT" --size=4G --randrepeat=0 \
        --output-format=json --output="$OUT/$name.json" --name="$name" "$@"
    summary "$OUT/$name.json" "$name"
}

# field 16 of /sys/block/<dev>/stat: flush requests completed
flushes() { awk '{print $16}' "/sys/block/ublkb$ID/stat"; }

run randread-qd1    --rw=randread --bs=4k --iodepth=1
run randread-qd32   --rw=randread --bs=4k --iodepth=32
run seqread-1m      --rw=read --bs=1M --iodepth=1
run seqwrite-1m-qd1 --rw=write --bs=1M --iodepth=1
run seqwrite-1m-qd4 --rw=write --bs=1M --iodepth=4
f0=$(flushes)
run randwrite-qd1   --rw=randwrite --bs=4k --iodepth=1
timed_flush() {  # label: one flush of whatever the previous job left dirty
    python3 - "$DEV" "$1" <<'EOF'
import os, sys, time
fd = os.open(sys.argv[1], os.O_WRONLY)
t = time.monotonic(); os.fsync(fd); t = time.monotonic() - t
print(f"flush after {sys.argv[2]}: {t:.2f} s")
EOF
}
# timed on its own so it doesn't land inside the next job
timed_flush randwrite-qd1
run randwrite-fsync --rw=randwrite --bs=4k --iodepth=1 --fsync=1
echo "flushes during write jobs: $(( $(flushes) - f0 ))"

# a reader next to a writer that fsyncs every write
fio --filename="$DEV" --direct=1 --ioengine=libaio \
    --time_based --runtime="$RT" --size=4G --randrepeat=0 \
    --output-format=json --output="$OUT/blocking.json" \
    --name=reader --rw=randread --bs=4k --iodepth=1 \
    --name=writer --rw=randwrite --bs=4k --iodepth=1 --fsync=1 >/dev/null
summary "$OUT/blocking.json" blocking

# readers keeping the actuator busy next to a cached writer: does the
# cache still get written back, or does it fill and stall the writer?
fio --filename="$DEV" --direct=1 --ioengine=libaio \
    --time_based --runtime="$RT" --size=4G --randrepeat=0 \
    --output-format=json --output="$OUT/read-vs-cache.json" \
    --name=reader --rw=randread --bs=4k --iodepth=4 \
    --name=writer --rw=randwrite --bs=4k --iodepth=1 >/dev/null
summary "$OUT/read-vs-cache.json" read-vs-cache
timed_flush read-vs-cache

# a reader next to a sequential writer: how long does a read wait behind
# write-back of a long run of cached data?
fio --filename="$DEV" --direct=1 --ioengine=libaio \
    --time_based --runtime="$RT" --size=4G --randrepeat=0 \
    --output-format=json --output="$OUT/read-vs-seq.json" \
    --name=reader --rw=randread --bs=4k --iodepth=1 \
    --name=writer --rw=write --bs=1M --iodepth=4 >/dev/null
summary "$OUT/read-vs-seq.json" read-vs-seq
timed_flush read-vs-seq

cat "$OUT/model.stats"
echo "results in $OUT"
