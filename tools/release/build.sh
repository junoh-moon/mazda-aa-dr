#!/bin/sh
# Release stage 1 (G1): pinned GCC 4.9.1 toolchain ARM build of the six
# artifacts in WORK/arm with arm-build.json (release_verified inputs).
#   sh tools/release/build.sh [WORK]
# MX5_TOOLCHAIN=DIR copies an existing pinned toolchain into the clone (the
# build records its blob hashes); otherwise tools/fetch_m3_toolchain.py fetches it.
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
rel_work "${1:-}"
stage_begin build
set -e
cd "$SRC"
test "$(git rev-parse HEAD)" = "$COMMIT"
if [ ! -d tools/m3-toolchain ]; then
    if [ -n "${MX5_TOOLCHAIN:-}" ]; then cp -a "$MX5_TOOLCHAIN" tools/m3-toolchain
    else python3 tools/fetch_m3_toolchain.py --jobs "${REL_JOBS:-8}"; fi
fi
[ -z "$(git status --porcelain | grep -v '^?? tools/m3-toolchain')" ] || { echo 'source tree changed'; exit 1; }
[ ! -e "$WORK/arm" ] || { echo "$WORK/arm exists: never reuse a build directory"; exit 1; }
wait_load
nice -n "$REL_NICE" python3 tools/build_arm.py --toolchain "$PWD/tools/m3-toolchain" --build-dir "$WORK/arm" \
    > "$WORK/evidence/arm-build.txt" 2>&1
tail -5 "$WORK/evidence/arm-build.txt"
for f in libmx5dr.so libmx5dr-vimtap.so libmx5dr-ldstap.so mx5dr-collector mx5dr-guard mx5dr-sha256; do
    test -s "$WORK/arm/$f"
    sha256sum "$WORK/arm/$f" | sed "s#  .*/#  #"
done | tee "$WORK/evidence/artifacts.sha256"
evidence arm-build.txt; evidence artifacts.sha256
