#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Lumir Sliva
# Calibration micro-benchmarks for a kublk ssd device.
#
# usage: sudo bench/calibrate_ssd.sh [runtime_s=30] [extra kublk ssd options...]
#   e.g. sudo bench/calibrate_ssd.sh 30 --profile nvme-plp
#
# Creates a memory-backed null_blk (4 GiB, configfs name ublkssd<ID>) as the
# backing store and measures the host's overhead first: 4K random read
# latency at QD1 through the ssd target set up as a fixed 100 us device,
# minus 100 us, minus the same read on the null_blk alone. It goes through
# the model's own timers, so it includes the server thread waking up for
# them. That is passed as --floor_us (set FLOOR_US to skip the
# measurement). Then it starts `kublk add -t ssd` on the null_blk as
# /dev/ublkb<ID>, runs the fio jobs below one at a time, prints one
# summary line per job and tears everything down. Queue depth is 128 if
# the options mention nvme, else 32 (override with DEPTH). Results (fio
# json+ with the full latency histograms, results.tsv, model stats) go to
# $OUT. With only --profile <p>, the
# run ends with bench/check.py against bench/expect/<p>.tsv and exits
# non-zero if a number is out of tolerance (EXPECT=<name> picks another
# expectation file, EXPECT=none skips the check).
#
# REAL=<block device> runs the same jobs on a real drive instead, so one
# expectation file judges the drive and its model:
#   sudo REAL=/dev/nvme0n1p1 bench/calibrate_ssd.sh 30 --profile micron-7300
# The options then only pick the expectations. It WRITES to the first
# 4 GiB of the device (filled with 1M writes first, so reads hit mapped
# data). It refuses a device smaller than 4 GiB + 1 MiB, one with a filesystem
# signature or partitions, and any partition of a drive that has
# something mounted or held (a system disk's boot partitions included).
set -euo pipefail

RT=${1:-30}; shift || true
ID=${ID:-12}
OUT=${OUT:-/tmp/ublk-disksim-cal-ssd-$(date +%s)}
HERE=$(cd "$(dirname "$0")/.." && pwd)
REAL=${REAL:-}
SKIP=0     # offset of every fio job
case " $* $REAL " in *nvme*|*micron-7300*) DEPTH=${DEPTH:-128} ;; *) DEPTH=${DEPTH:-32} ;; esac
mkdir -p "$OUT"

if [ -n "$REAL" ]; then
    DEV=$REAL
    [ -b "$DEV" ] || { echo "REAL=$DEV: not a block device" >&2; exit 1; }
    NAME=$(basename "$(readlink -f "$DEV")")
    DISK=$(lsblk -ndo PKNAME "$DEV")
    DISK=/dev/${DISK:-$NAME}
    held=
    for n in $(lsblk -nro KNAME "$DISK"); do
        held=$held$(ls "/sys/class/block/$n/holders")
    done
    if [ "$(blockdev --getsize64 "$DEV")" -lt $(((4 << 30) + (1 << 20))) ] ||
       [ -n "$(blkid -p -o value -s TYPE "$DEV" || true)" ] ||
       [ "$(lsblk -nr "$DEV" | wc -l)" -gt 1 ] ||
       [ -n "$(lsblk -nro MOUNTPOINT "$DISK" | tr -d '\n')" ] ||
       [ -n "$held" ]; then
        echo "REAL=$DEV: under 4 GiB, has a filesystem or partitions, or" \
             "its drive $DISK has something mounted or held: not writing" \
             "to it" >&2
        exit 1
    fi
    lsblk -s -o NAME,SIZE,MODEL,SERIAL "$DEV" | tee "$OUT/device.txt"
    # the jobs start 1 MiB in: random data at sector 0 can pass for a
    # partition table (the kernel's Atari parser checks almost nothing)
    SKIP=1M
    fio --name=fill --filename="$DEV" --offset=$SKIP --rw=write --bs=1M \
        --size=4G --direct=1 --ioengine=libaio --iodepth=32 --output=/dev/null
else
    CFG=/sys/kernel/config/nullb/ublkssd$ID
    BACK=/dev/ublkssd$ID    # null_blk configfs devices are named after the dir
    DEV=/dev/ublkb$ID
    DISK=/dev/ublkb$ID

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
    # fill once so reads hit written data
    fio --name=fill --filename="$BACK" --rw=write --bs=1M --size=1G --direct=1 \
        --ioengine=libaio --iodepth=4 --output=/dev/null

    mean_lat_us() {  # device: mean 4K random read latency at QD1, us
        fio --name=floor --filename="$1" --rw=randread --bs=4k --iodepth=1 \
            --direct=1 --ioengine=io_uring --time_based --runtime=10 \
            --size=1G --randrepeat=0 --output-format=json |
            python3 -c 'import json,sys; print(json.load(sys.stdin)["jobs"][0]["read"]["lat_ns"]["mean"]/1e3)'
    }

    if [ -z "${FLOOR_US:-}" ]; then
        raw=$(mean_lat_us "$BACK")
        "$HERE/kublk" add -t ssd -n "$ID" -q 1 -d "$DEPTH" --iface nvme \
            --tr_us 100 --iface_us 0 --cmd_us 0 --ch_mbps 1e6 --iface_mbps 1e6 \
            --floor_us 0 \
            "$BACK" >/dev/null
        ADDED=1
        udevadm settle
        fixed=$(mean_lat_us "$DEV")
        "$HERE/kublk" del -n "$ID"
        ADDED=
        FLOOR_US=$(python3 -c "print(max(0, round($fixed - 100 - $raw, 1)))")
        echo "floor: fixed 100 us device $fixed us - 100 - null_blk $raw us = $FLOOR_US us"
    fi

    "$HERE/kublk" add -t ssd -n "$ID" -q 1 -d "$DEPTH" --floor_us "$FLOOR_US" \
        --stats "$OUT/model.stats" "$@" "$BACK" | tee "$OUT/kublk.txt"
    ADDED=1
    udevadm settle
fi
# the whole drive: it holds the queue settings, and a partition's own
# stat file may miss the flushes its fsyncs cause
SYS=/sys/class/block/${DISK#/dev/}
echo "device $DEV: depth $DEPTH rotational $(cat "$SYS/queue/rotational")" \
     "write_cache '$(cat "$SYS/queue/write_cache")'"

summary() {  # fio json, label
    python3 "$HERE/bench/summary.py" "$1" "$2" us "$OUT/results.tsv" \
        "$(cat "$SYS/queue/write_cache")"
}

run() {  # name, fio args...
    local name=$1; shift
    fio --filename="$DEV" --offset=$SKIP --direct=1 --ioengine=io_uring \
        --time_based --runtime="$RT" --size=4G --randrepeat=0 \
        --lat_percentiles=1 --output-format=json+ --output="$OUT/$name.json" --name="$name" "$@"
    summary "$OUT/$name.json" "$name"
}

# field 16 of /sys/class/block/<drive>/stat: flush requests completed
flushes() { awk '{print $16}' "$SYS/stat"; }

run randread-qd1     --rw=randread --bs=4k --iodepth=1
run randread-qd32    --rw=randread --bs=4k --iodepth=32
[ "$DEPTH" -ge 128 ] && run randread-qd128 --rw=randread --bs=4k --iodepth=128
run seqread-128k     --rw=read --bs=128k --iodepth=32
run seqwrite-128k    --rw=write --bs=128k --iodepth=32
run randwrite-qd1    --rw=randwrite --bs=4k --iodepth=1
run randwrite-qd32   --rw=randwrite --bs=4k --iodepth=32
f0=$(flushes)
run randwrite-fsync  --rw=randwrite --bs=4k --iodepth=1 --fsync=1
# a WAL: 4K appends, each followed by fsync
run seqwrite-fsync   --rw=write --bs=4k --iodepth=1 --fsync=1
nf=$(( $(flushes) - f0 ))
echo "flushes during fsync jobs: $nf"
printf 'fsync-jobs\t-\t-\tdevice_flushes\t%s\n' "$nf" >> "$OUT/results.tsv"

# a reader next to a writer that fsyncs every write
fio --filename="$DEV" --offset=$SKIP --direct=1 --ioengine=io_uring \
    --time_based --runtime="$RT" --size=4G --randrepeat=0 \
    --lat_percentiles=1 --output-format=json+ --output="$OUT/blocking.json" \
    --name=reader --rw=randread --bs=4k --iodepth=1 \
    --name=writer --rw=randwrite --bs=4k --iodepth=1 --fsync=1 >/dev/null
summary "$OUT/blocking.json" blocking

# a bulk random writer keeping the buffer full, next to a writer that
# fsyncs every write, on separate halves of the device
fio --filename="$DEV" --direct=1 --ioengine=io_uring \
    --time_based --runtime="$RT" --randrepeat=0 \
    --lat_percentiles=1 --output-format=json+ --output="$OUT/full-fsync.json" \
    --name=bulk --rw=randwrite --bs=4k --iodepth=16 --offset=$SKIP --size=2G \
    --name=fsyncer --rw=randwrite --bs=4k --iodepth=1 --fsync=1 \
    --offset=2G --size=2G >/dev/null
summary "$OUT/full-fsync.json" full-fsync
echo "flushes seen by the kernel: $(flushes)"
STATS=
if [ -z "$REAL" ]; then
    STATS=$OUT/model.stats    # the model's own counts; final after teardown
    cat "$STATS"
fi
echo "results in $OUT"

# which expectations: a stock profile alone (none given = sata-plp)
set -- $*
case "$#:${1:-}" in
0:) auto=sata-plp ;;
2:--profile) auto=$2 ;;
*) auto=none ;;
esac
expect=${EXPECT:-$auto}
if [ -n "${EXPECT:-}" ] && [ "$EXPECT" != none ] &&
   [ ! -f "$HERE/bench/expect/$EXPECT.tsv" ]; then
    echo "check: no bench/expect/$EXPECT.tsv" >&2
    exit 1
elif [ "$expect" = none ] || [ ! -f "$HERE/bench/expect/$expect.tsv" ]; then
    echo "check: skipped (no expectations for these parameters; EXPECT=<name> to force)"
else
    python3 "$HERE/bench/check.py" "$OUT/results.tsv" \
        "$HERE/bench/expect/$expect.tsv" ${STATS:+"$STATS"}
fi
