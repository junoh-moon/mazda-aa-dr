#!/bin/sh
# Release stage 5 (G2 packaging, BDS parse check): tests/packaging against the
# release bundle and stock root. Fails closed on failure, on any skip (unless
# REL_ALLOW_PACK_SKIP=1) and when the original-bds parse lines are missing.
#   sh tools/release/pack.sh [WORK]
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
rel_work "${1:-}"
stage_begin pack
need MX5DR_STOCK_ROOT
toolchain_env; qemu_env
cd "$SRC" || exit 1
out=$WORK/evidence/packaging-tests.txt
wait_load
MX5DR_STOCK_ROOT=$MX5DR_STOCK_ROOT MX5DR_RELEASE_BUNDLE=$WORK/unpacked MX5DR_TEST_BUILD=$SRC/build \
    nice -n "$REL_NICE" python3 -m unittest discover -s tests/packaging -v > "$out" 2>&1
e=$?; echo "EXIT=$e" >> "$out"
tail -3 "$out"
evidence packaging-tests.txt
[ "$e" = 0 ] || exit 1
if grep -Eq 'skipped=[1-9]' "$out" && [ "${REL_ALLOW_PACK_SKIP:-0}" != 1 ]; then echo 'packaging skips are not a pass'; exit 1; fi
for l in 'bds parse check: radio_open count=8' 'bds parse check (wcp): radio_open count=8'; do
    grep -qF "$l" "$out" || { echo "missing: $l"; exit 1; }
done
