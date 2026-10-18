#!/bin/sh
# Release stage 4 (G2): host `make -k test` with the stock root and the
# release bundle. Records exit, unittest totals and WORKER_BETA_TIMING_RETRY.
#   sh tools/release/host.sh [WORK]
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
rel_work "${1:-}"
stage_begin host
need MX5DR_STOCK_ROOT
cd "$SRC" || exit 1
flags=${HOST_DBUS_FLAGS:-$(pkg-config --cflags dbus-1 2>/dev/null)}
libs=${HOST_DBUS_LIBS:-$(pkg-config --libs dbus-1 2>/dev/null)}
out=$WORK/evidence/host-tests.txt
wait_load
MX5DR_STOCK_ROOT=$MX5DR_STOCK_ROOT MX5DR_RELEASE_BUNDLE=$WORK/unpacked \
    nice -n "$REL_NICE" make -k HOST_DBUS_FLAGS="$flags" HOST_DBUS_LIBS="$libs" test > "$out" 2>&1
e=$?; echo "EXIT=$e" >> "$out"
echo "unittest totals: $(grep -E '^Ran [0-9]+ tests' "$out" | awk '{s+=$2} END {print s+0}') tests," \
     "$(grep -Eo 'skipped=[0-9]+' "$out" | cut -d= -f2 | awk '{s+=$1} END {print s+0}') skipped," \
     "FAILED blocks $(grep -c '^FAILED' "$out")"
echo "WORKER_BETA_TIMING_RETRY lines: $(grep -c WORKER_BETA_TIMING_RETRY "$out")"
evidence host-tests.txt
exit $e
