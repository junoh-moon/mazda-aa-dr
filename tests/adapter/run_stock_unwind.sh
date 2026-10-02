#!/bin/sh
# Developer PC only. Pin the private OEM loader/runtime around an authored ARM
# DSO test. The output directory contains private paths and guest loader logs.
set -eu
umask 077
: "${CROSS_COMPILE:?Set the pinned ARM compiler prefix}"
: "${MX5DR_STOCK_ROOT:?Set the private original rootfs}"
: "${MX5DR_ARM_LIBRARY:?Set the production AA DSO}"
project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
stock=$(CDPATH= cd -- "$MX5DR_STOCK_ROOT" && pwd)
library=$(CDPATH= cd -- "$(dirname -- "$MX5DR_ARM_LIBRARY")" && pwd)/$(basename -- "$MX5DR_ARM_LIBRARY")
build=${MX5DR_STOCK_UNWIND_BUILD:-$project/build/stock-unwind}
mkdir -p "$build"
build=$(CDPATH= cd -- "$build" && pwd)
qemu=$(command -v qemu-arm)
command -v "${CROSS_COMPILE}g++" >/dev/null
command -v "${CROSS_COMPILE}nm" >/dev/null
command -v sha256sum >/dev/null
command -v jq >/dev/null
for path in lib/ld-linux.so.3 lib/libc.so.6 lib/libpthread.so.0 \
            lib/libgcc_s.so.1 lib/libdl.so.2 lib/libm.so.6 \
            lib/librt.so.1 usr/lib/libstdc++.so.6; do
    [ -r "$stock/$path" ] || { echo "Missing private runtime: $path" >&2; exit 1; }
done
[ -r "$library" ] || { echo 'Missing AA DSO' >&2; exit 1; }
unset LD_PRELOAD LD_LIBRARY_PATH LD_AUDIT LD_DEBUG QEMU_SET_ENV QEMU_UNSET_ENV QEMU_LD_PREFIX
identity() {
    sha256sum "$qemu" "${CROSS_COMPILE}g++" \
        "$stock/lib/ld-linux.so.3" "$stock/lib/libc.so.6" \
        "$stock/lib/libpthread.so.0" "$stock/lib/libgcc_s.so.1" \
        "$stock/lib/libdl.so.2" "$stock/lib/libm.so.6" \
        "$stock/lib/librt.so.1" \
        "$stock/usr/lib/libstdc++.so.6" "$library" \
        "$project/tests/adapter/run_stock_unwind.sh"
}
rm -f "$build/identity.after.sha256" "$build/lddebug.log" "$build/success.txt"
identity > "$build/identity.before.sha256"
cd "$project"
python3 tests/adapter/run_unwind_dso.py --library "$library" \
    --cross-prefix "$CROSS_COMPILE" --sysroot "$stock" \
    --suite position --output-dir "$build"
qemu-arm -L "$stock" -E "LD_PRELOAD=$library" \
    -E "MX5_UNWIND_LIBRARY=$library" -E LD_DEBUG=libs \
    "$build/unwind-dso-test" small_stack_cancel > "$build/lddebug.log" 2>&1
grep -Fq 'PASS ARM unwind small_stack_cancel:' "$build/lddebug.log"
for path in /lib/libpthread.so.0 /lib/libc.so.6 \
            /lib/libgcc_s.so.1 /lib/libdl.so.2 /lib/libm.so.6 \
            /lib/librt.so.1 /usr/lib/libstdc++.so.6; do
    grep -Fq "calling init: $path" "$build/lddebug.log" || {
        echo "Guest library not confirmed: $path" >&2; exit 1;
    }
done
identity > "$build/identity.after.sha256"
cmp "$build/identity.before.sha256" "$build/identity.after.sha256"
recorded_exe=$(jq -r .executable_sha256 "$build/unwind-dso.json")
actual_exe=$(sha256sum "$build/unwind-dso-test" | awk '{print $1}')
[ "$recorded_exe" = "$actual_exe" ] || {
    echo 'Authored executable changed after guest run' >&2; exit 1;
}
sha256sum "$build/unwind-dso.json" "$build/unwind-dso-test" \
    "$build/identity.before.sha256" \
    "$build/lddebug.log" > "$build/success.txt"
printf 'PASS stock loader identity and authored AA unwind; record=%s\n' "$build/success.txt"
