#!/bin/sh
# G10 soak: (1) one long-lived ARM (qemu-arm) process running the real BETA
# runtime worker with repeated tunnel-style GPS loss/return cycles and a small
# rotating journal (tests/gates/soak_worker.cpp), then (2) N short boots: the
# exact-ARM worker-beta-test scenarios as separate processes (every process is
# a fresh worker start, journal and BETA arm), cycling through the scenario
# list. Time-scalable.
#
#   sh tests/gates/g10_soak.sh [--fast] [WORK]
#
# Environment (defaults):
#   SOAK_SECONDS   long-run length in seconds (14400 = 4 h; --fast: 420)
#   SOAK_BOOTS     number of short boots (100; --fast: 12)
#   SOAK_LOG_BYTES journal max_log_bytes of the long run (262144, x3 files)
#   SOAK_NICE      nice level of the qemu processes (10)
#   SOAK_MAX_LOAD  wait before the long run and each boot while the 1-min
#                  load is >= this (4); the load is logged with every result
#   SOAK_HOST=1    build and run natively instead of ARM (development only;
#                  not gate evidence)
#   MX5_TOOLCHAIN  pinned toolchain (REPO/tools/m3-toolchain); CROSS_COMPILE
#                  and QEMU_SYSROOT override its prefix and sysroot
#   QEMU_ARM       qemu-arm (qemu-arm on PATH)
#   SOAK_SCENARIOS short-boot scenarios (main budget nofix silence contradict
#                  fault no_anchor withdrawn_class cadence_gap nofix_rearm disable)
# Output: WORK/soak.log with the long-run JSON line, per-boot exit codes,
# wall time and peak RSS (kB, of the qemu process), and a final
# "G10: PASS|FAIL ..." line. Exit 0 pass, 1 fail, 77 skip (no toolchain/qemu).
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/../.." && pwd)
fast=0
[ "${1:-}" = --fast ] && { fast=1; shift; }
work=${1:-$(mktemp -d "${TMPDIR:-/tmp}/mx5dr-g10.XXXXXX")}
mkdir -p "$work" || exit 1
work=$(CDPATH= cd -- "$work" && pwd)
if [ "$fast" = 1 ]; then seconds=${SOAK_SECONDS:-420}; boots=${SOAK_BOOTS:-12}
else seconds=${SOAK_SECONDS:-14400}; boots=${SOAK_BOOTS:-100}; fi
logbytes=${SOAK_LOG_BYTES:-262144}
nicelvl=${SOAK_NICE:-10}
scenarios=${SOAK_SCENARIOS:-main budget nofix silence contradict fault no_anchor withdrawn_class cadence_gap nofix_rearm disable}
log=$work/soak.log
: > "$log"
say() { echo "$*" | tee -a "$log"; }
maxload=${SOAK_MAX_LOAD:-4}
wait_load() { while [ "$(cut -d. -f1 /proc/loadavg)" -ge "$maxload" ]; do sleep 10; done; }
cd "$repo" || exit 1
srcs='src/runtime/assist_worker.cpp src/runtime/config.cpp src/runtime/sha256.cpp src/runtime/lds_sideband.cpp src/runtime/lds_request_source.cpp src/runtime/lds_association_channel.cpp src/adapter/adapter.cpp src/adapter/arm_entry.cpp src/adapter/v74_install.cpp src/adapter/bus_hooks.cpp src/adapter/session_hooks.cpp src/adapter/request_hooks.cpp src/runtime/request_observer.cpp src/runtime/request_trace.cpp src/navigation/pipeline.cpp src/navigation/channel.cpp src/navigation/holdout.cpp src/runtime/core_bridge.cpp src/runtime/loader.cpp'
if [ "${SOAK_HOST:-0}" = 1 ]; then
    cc=cc; cxx=g++; arch=''; run=''
    say 'SOAK_HOST=1: native build (development only, not gate evidence)'
else
    tc=${MX5_TOOLCHAIN:-$repo/tools/m3-toolchain}
    CROSS_COMPILE=${CROSS_COMPILE:-$tc/bin/arm-cortexa9_neon-linux-gnueabi-}
    QEMU_SYSROOT=${QEMU_SYSROOT:-$tc/arm-cortexa9_neon-linux-gnueabi/sysroot}
    qemu=${QEMU_ARM:-$(command -v qemu-arm || true)}
    if [ ! -x "${CROSS_COMPILE}g++" ] || [ ! -d "$QEMU_SYSROOT" ] || [ -z "$qemu" ]; then
        echo 'SKIP: pinned ARM toolchain or qemu-arm not found (MX5_TOOLCHAIN, CROSS_COMPILE, QEMU_SYSROOT, QEMU_ARM)'; exit 77
    fi
    cc=${CROSS_COMPILE}gcc; cxx=${CROSS_COMPILE}g++
    arch='-march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp -marm'
    srcs="$srcs src/adapter/arm_veneer.S src/adapter/request_veneer.S"
    run="$qemu -L $QEMU_SYSROOT"
fi
warn='-O2 -Wall -Wextra -Werror'
b=$work/build
mkdir -p "$b"
say "G10 soak: seconds=$seconds boots=$boots log_bytes=$logbytes nice=$nicelvl commit=$(git rev-parse HEAD 2>/dev/null) dirty=$(git status --porcelain -- src tests | wc -l)"
# shellcheck disable=SC2086
"$cc" -std=c99 $warn -pedantic $arch -Isrc/core -c src/core/dr_core.c -o "$b/core.o" &&
"$cxx" -std=c++11 $warn $arch -Isrc $srcs "$b/core.o" tests/gates/soak_worker.cpp -ldl -pthread -lrt -lm -o "$b/soak-worker" &&
"$cxx" -std=c++11 $warn $arch -Isrc $srcs "$b/core.o" tests/runtime/test_worker_beta.cpp -ldl -pthread -lrt -lm -o "$b/worker-beta-test" ||
    { say 'G10: FAIL (build)'; exit 1; }
rc=0
wait_load
say "== long run ($seconds s) start $(date +%H:%M:%S) load $(cut -d' ' -f1-3 /proc/loadavg)"
mkdir -p "$work/run"
# shellcheck disable=SC2086
nice -n "$nicelvl" /usr/bin/time -f 'LONG_TIME wall_s=%e max_rss_kb=%M' $run "$b/soak-worker" "$seconds" "$logbytes" "$work/run" >> "$log" 2>&1
long=$?
say "== long run exit $long end $(date +%H:%M:%S) load $(cut -d' ' -f1-3 /proc/loadavg)"
[ "$long" = 0 ] || rc=1
say "== $boots short boots start $(date +%H:%M:%S)"
n=0; ok=0; timing=0; failed=''
while [ "$n" -lt "$boots" ]; do
    for s in $scenarios; do
        [ "$n" -lt "$boots" ] || break
        n=$((n + 1))
        out=$work/boot-$n-$s.txt
        wait_load
        # shellcheck disable=SC2086
        nice -n "$nicelvl" /usr/bin/time -f 'BOOT_TIME wall_s=%e max_rss_kb=%M' $run "$b/worker-beta-test" "$s" > "$out" 2>&1
        e=$?
        t=$(grep -o 'BOOT_TIME.*' "$out" | tail -1)
        echo "BOOT $n scenario=$s exit=$e $t load=$(cut -d' ' -f1 /proc/loadavg)" >> "$log"
        case $e in 0) ok=$((ok + 1));; 77) timing=$((timing + 1)); failed="$failed $n:$s(77)";; *) failed="$failed $n:$s($e)";; esac
    done
done
say "== short boots: $ok/$boots passed, timing-inconclusive (exit 77, lag/summary-gap check) $timing, failed:${failed:- none}"
[ "$ok" = "$boots" ] || rc=1
rss=$(grep -o 'max_rss_kb=[0-9]*' "$log" | grep -A0 . | sed -n '2,$p' | cut -d= -f2 | sort -n | sed -n '1p;$p' | tr '\n' ' ')
say "short-boot qemu peak RSS min/max kB: $rss"
json=$(grep '^{"soak_seconds"' "$log" | tail -1)
say "long run: $json"
if [ "$rc" = 0 ]; then say "G10: PASS (long run $seconds s, $boots short boots)"; else say "G10: FAIL (see $log)"; fi
exit $rc
