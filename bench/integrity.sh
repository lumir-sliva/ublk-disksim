#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 Lumir Sliva
# Data-integrity check for the kublk disk models.
#
# usage: sudo bench/integrity.sh [runtime_s=20]
#
# The models only delay completions: they never look at, buffer, drop or
# reorder the data itself, which goes straight to the backing store.
# This proves it. For every target and cache mode it creates a fresh
# memory-backed null_blk (1 GiB, configfs name ublkint<ID>), puts a model
# device on it as /dev/ublkb<ID> and runs fio jobs that verify a crc32c
# of every block they wrote, under flushes, concurrent I/O and mixed
# block sizes, plus a raw dd + sha256 pass. One PASS/FAIL line per device
# and job; the exit status is the number of failed jobs.
#
# NEGATIVE=1 is the control for the check itself: it runs one device and
# only job (a), and zeroes the backing store between the write and the
# verify. The expected result there is a FAIL.
set -euo pipefail

RT=${1:-20}
ID=${ID:-11}
OUT=${OUT:-/tmp/ublk-disksim-integrity-$(date +%s)}
HERE=${HERE:-$(cd "$(dirname "$0")/.." && pwd)}
KUBLK=${KUBLK:-$HERE/kublk}
CFG=/sys/kernel/config/nullb/ublkint$ID
BACK=/dev/ublkint$ID    # null_blk configfs devices are named after the dir
DEV=/dev/ublkb$ID
SEED=${SEED:-1234567}   # the write and the verify pass must draw the same offsets
mkdir -p "$OUT"
cd "$OUT"               # --verify_dump drops the mismatching blocks into cwd

# One region per job on the 1 GiB device, so no job ever reads a header
# another one wrote (they use different block sizes).
OFF_A=0; OFF_B=256m; OFF_C=512m; OFF_D=768m

cleanup() {  # only what this run created
    dev_down
    back_down
}

back_up() {
    mkdir "$CFG"        # fails if another run uses this ID: nothing to undo
    MADE=1
    trap cleanup EXIT
    for kv in size=1024 blocksize=512 memory_backed=1 irqmode=0 queue_mode=2; do
        echo "${kv#*=}" > "$CFG/${kv%%=*}"
    done
    echo 1 > "$CFG/power"
}

back_down() {
    [ -n "${MADE:-}" ] || return 0
    if [ -d "$CFG" ]; then
        echo 0 > "$CFG/power"
        rmdir "$CFG"
    fi
    MADE=
}

dev_up() {  # queue depth, model options...
    local depth=$1; shift
    "$KUBLK" add -t "$TARGET" -n "$ID" -q 1 -d "$depth" "$@" "$BACK" \
        > "$OUT/$LABEL.kublk.txt"
    ADDED=1
    udevadm settle
}

dev_down() {
    [ -n "${ADDED:-}" ] || return 0
    "$KUBLK" del -n "$ID" >/dev/null 2>&1 || true
    ADDED=
}

PASSED=0
FAILED=0

verdict() {  # job name, status, seconds, log
    if [ "$2" -eq 0 ]; then
        printf 'PASS  %-14s %-22s %4ds\n' "$LABEL" "$1" "$3"
        PASSED=$((PASSED + 1))
    else
        printf 'FAIL  %-14s %-22s %4ds  %s\n' "$LABEL" "$1" "$3" "$4"
        grep -im4 -E 'verify|bad|corrupt|error' "$4" | sed 's/^/      /' || true
        FAILED=$((FAILED + 1))
    fi
}

run_job() {  # job name, fio args...
    local name=$1; shift
    local log=$OUT/$LABEL.$name.log t0=$SECONDS st=0
    fio --filename="$DEV" --direct=1 --ioengine="$ENGINE" --randseed="$SEED" \
        --verify=crc32c --do_verify=1 --verify_fatal=1 --verify_dump=1 \
        --verify_state_save=0 --name="$name" "$@" > "$log" 2>&1 || st=$?
    verdict "$name" "$st" "$((SECONDS - t0))" "$log"
}

raw_pass() {  # a known pattern through the model and back out again
    local log=$OUT/$LABEL.raw-dd.log t0=$SECONDS st=0 want got
    dd if="$OUT/pattern.bin" of="$DEV" bs=1M oflag=direct conv=fsync \
        status=none || st=$?
    dd if="$DEV" of="$OUT/readback.bin" bs=1M count="$PATTERN_MB" \
        iflag=direct status=none || st=$?
    want=$(sha256sum < "$OUT/pattern.bin" | cut -d' ' -f1)
    got=$(sha256sum < "$OUT/readback.bin" | cut -d' ' -f1)
    { echo "dd status $st"; echo "wrote $want"; echo "read  $got"; } > "$log"
    [ "$st" -eq 0 ] && [ "$want" = "$got" ] || st=1
    verdict raw-dd-sha256 "$st" "$((SECONDS - t0))" "$log"
}

device_jobs() {
    # (a) 4K random writes with a flush every 8 of them, then a verify
    #     pass over the same blocks. Two fio runs rather than one, so the
    #     negative control has somewhere to corrupt the backing store.
    local a=(--rw=randwrite --bs=4k --iodepth=16 --offset=$OFF_A --size=$RSZ)
    run_job a-randwrite-fsync-write "${a[@]}" --fsync=8 --do_verify=0
    if [ -n "${NEGATIVE:-}" ]; then
        echo "      negative control: zeroing ${RSZ_MB} MiB of $BACK at offset 0"
        dd if=/dev/zero of="$BACK" bs=1M count="$RSZ_MB" oflag=direct \
            conv=fsync status=none
    fi
    run_job a-randwrite-fsync-verify "${a[@]}" --verify_only=1
    [ -z "${NEGATIVE:-}" ] || return 0

    # (b) sequential 1M writes, verified afterwards
    run_job b-seqwrite-1m --rw=write --bs=1M --iodepth=4 \
        --offset=$OFF_B --size=256m

    # (c) mixed random read/write with verification running inside the
    #     job. The region is laid down first so that the reads of the
    #     workload find a header wherever they land.
    run_job c-prefill --rw=write --bs=4k --iodepth=16 \
        --offset=$OFF_C --size=$RSZ --do_verify=0
    run_job c-randrw-verify --rw=randrw --rwmixread=70 --bs=4k --iodepth=8 \
        --offset=$OFF_C --size=$RSZ --verify_backlog=64 \
        --time_based --runtime="$RT"

    # (d) every block size between 512 B and 128 KiB, 512-aligned
    run_job d-mixed-bs --rw=randwrite --bsrange=512-128k --blockalign=512 \
        --iodepth=8 --offset=$OFF_D --size=$MSZ

    # and the same thing without fio: a known pattern, written with a
    # flush, read back, compared by hash
    raw_pass
}

run_device() {  # label, target, queue depth, model options...
    LABEL=$1 TARGET=$2
    local depth=$3; shift 3
    case $TARGET in
    # the hdd does ~80 random IOPS, so its random jobs cover much less
    # ground than the ssd's; sequential jobs are the same on both
    hdd) ENGINE=libaio;   RSZ_MB=${HDD_RSZ_MB:-8};    MSZ_MB=${HDD_MSZ_MB:-32} ;;
    ssd) ENGINE=io_uring; RSZ_MB=${SSD_RSZ_MB:-256};  MSZ_MB=${SSD_MSZ_MB:-256} ;;
    esac
    RSZ=${RSZ_MB}m MSZ=${MSZ_MB}m
    back_up
    dev_up "$depth" "$@"
    printf '\n== %-14s %s -d %-3s %s (rotational %s, write_cache "%s")\n' \
        "$LABEL" "$TARGET" "$depth" "$*" \
        "$(cat "/sys/block/ublkb$ID/queue/rotational")" \
        "$(cat "/sys/block/ublkb$ID/queue/write_cache")"
    device_jobs
    dev_down
    back_down
}

modprobe null_blk nr_devices=0 2>/dev/null || true
modprobe ublk_drv
mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config

PATTERN_MB=64
dd if=/dev/urandom of="$OUT/pattern.bin" bs=1M count="$PATTERN_MB" status=none

if [ -n "${NEGATIVE:-}" ]; then
    run_device hdd-cache64 hdd 32 --cache_mb 64
    echo
    echo "negative control: the FAIL above is the expected result"
else
    run_device hdd-cache64   hdd 32  --cache_mb 64
    run_device hdd-nocache   hdd 32  --cache_mb 0
    run_device barracuda-2t  hdd 32  --profile barracuda-2t
    run_device sata-plp      ssd 32  --profile sata-plp
    run_device nvme-plp      ssd 128 --profile nvme-plp
    run_device sata-consumer ssd 32  --profile sata-consumer
    run_device micron-7300   ssd 128 --profile micron-7300
    run_device nvme-vwc      ssd 128 --profile nvme-plp --vwc 1 --plp 0
fi

rm -f "$OUT/pattern.bin" "$OUT/readback.bin"
echo
echo "$PASSED passed, $FAILED failed; logs in $OUT"
[ "$FAILED" -eq 0 ] || exit $(( FAILED > 125 ? 125 : FAILED ))
