#!/bin/sh
# Release stage 3 (G3, G4): exact-ARM suite tests/run_arm_all.sh under
# qemu-arm with the original LDS runtime, the AA stock root and libpatch
# 0.10.0, then the AA install probe again with libpatch 0.9.1 (the vehicle's
# version). Fails closed on any non-zero exit, ARM_TIMING_FAILED != 0, a
# skipped AA probe (no libpatch) or a missing probe-0.9.1 input.
#   sh tools/release/arm.sh [WORK]
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
rel_work "${1:-}"
stage_begin arm
need MX5DR_STOCK_ROOT; need MX5DR_LIBPATCH; need MX5DR_LIBPATCH_091
toolchain_env; qemu_env
cd "$SRC" || exit 1
export MX5DR_ARM_BUILD="$WORK/arm"
export MX5DR_LDS_STOCK="${MX5DR_LDS_STOCK:-$MX5DR_STOCK_ROOT}"
export MX5DR_AA_STOCK="${MX5DR_AA_STOCK:-$MX5DR_STOCK_ROOT}"
export MX5DR_LIBPATCH MX5DR_PROOT="${MX5DR_PROOT:-$(command -v proot || true)}"
out=$WORK/evidence/arm-tests.txt
wait_load
nice -n "$REL_NICE" sh tests/run_arm_all.sh > "$out" 2>&1
e=$?; echo "EXIT=$e" >> "$out"
echo '== AA install probe with libpatch 0.9.1 (vehicle version)' >> "$out"
MX5DR_LIBPATCH=$MX5DR_LIBPATCH_091 nice -n "$REL_NICE" sh tests/adapter/run_aa_install_probe.sh >> "$out" 2>&1
p=$?; echo "PROBE091_EXIT=$p" >> "$out"
grep -E '^(ARM_TIMING_RETRIES|ARM_TIMING_FAILED)=|^EXIT=|^PROBE091_EXIT=' "$out"
evidence arm-tests.txt
[ "$e" = 0 ] && [ "$p" = 0 ] || exit 1
grep -q '^ARM_TIMING_FAILED=0$' "$out" || exit 1
if grep -qi 'aa install probe.*skip' "$out"; then echo 'AA install probe skipped'; exit 1; fi
