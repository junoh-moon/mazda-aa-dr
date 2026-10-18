#!/bin/sh
# Release stage 7 (G10): QEMU-ARM soak of the BETA runtime worker
# (tests/gates/g10_soak.sh). REL_SOAK=full (4 h + 100 short boots, default)
# or fast (7 min + 12 boots: a smoke check, not the gate).
#   sh tools/release/soak.sh [WORK]
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
rel_work "${1:-}"
stage_begin soak
toolchain_env; qemu_env
cd "$SRC" || exit 1
[ -f tests/gates/g10_soak.sh ] || { echo "the pinned commit has no tests/gates/g10_soak.sh"; exit 1; }
wait_load
flag=; [ "${REL_SOAK:-full}" = fast ] && flag=--fast
SOAK_NICE=$REL_NICE sh tests/gates/g10_soak.sh $flag "$WORK/gates/soak"
e=$?
cp "$WORK/gates/soak/soak.log" "$WORK/evidence/soak.txt" && evidence soak.txt
exit $e
