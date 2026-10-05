#!/bin/sh
# Developer PC only, optional: the real AA BLM and AA interface from the private stock root, the
# product preload and (optionally) the user's third-party oem-aa-mod patch, run in QEMU user mode
# under proot. Never a vehicle command and not a substitute for the vehicle.
#
# Why it exists: the BLM's session slots are interposed by libpatch-blmjciaapa.so (it exports
# aap_create_session/aap_destroy_session), so the installer must decline ONLY the session
# observation and still install the position hook. That combination was never run before the
# 2026-10-04 vehicle trial, where the whole hook was silently declined.
#
# Environment (all required, else exit 77 = SKIP):
#   MX5DR_AA_STOCK   private stock root containing jci/aapa/blmjciaapa.so, usr/lib/libaap_interface.so
#   MX5DR_LIBPATCH   a published oem-aa-mod libpatch-blmjciaapa.so (not stored in this repository)
#   MX5DR_ARM_BUILD  directory with the release libmx5dr.so (as for tests/run_arm_all.sh)
#   CROSS_COMPILE, QEMU_SYSROOT as for tests/run_arm_all.sh; proot (MX5DR_PROOT) and qemu-arm (QEMU_ARM)
set -eu
need() { eval "v=\${$1:-}"; [ -n "$v" ] || { echo "SKIP AA install probe: set $1" >&2; exit 77; }; }
need MX5DR_AA_STOCK; need MX5DR_LIBPATCH; need MX5DR_ARM_BUILD; need CROSS_COMPILE
proot=${MX5DR_PROOT:-proot}; qemu=${QEMU_ARM:-qemu-arm}
for f in "$MX5DR_AA_STOCK/jci/aapa/blmjciaapa.so" "$MX5DR_AA_STOCK/usr/lib/libaap_interface.so" \
         "$MX5DR_LIBPATCH" "$MX5DR_ARM_BUILD/libmx5dr.so"; do
    [ -r "$f" ] || { echo "SKIP AA install probe: missing $f" >&2; exit 77; }
done
command -v "$proot" >/dev/null || { echo 'SKIP AA install probe: proot not found' >&2; exit 77; }
command -v "$qemu" >/dev/null || { echo 'SKIP AA install probe: qemu-arm not found' >&2; exit 77; }
command -v "${CROSS_COMPILE}gcc" >/dev/null || { echo 'SKIP AA install probe: cross gcc not found' >&2; exit 77; }
unset MAKEFLAGS GNUMAKEFLAGS MFLAGS MAKEOVERRIDES MAKEFILES MAKELEVEL GCC_EXEC_PREFIX COMPILER_PATH \
    LIBRARY_PATH CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LD_RUN_PATH LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT \
    QEMU_SET_ENV QEMU_UNSET_ENV QEMU_LD_PREFIX
project=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
stock=$(CDPATH= cd -- "$MX5DR_AA_STOCK" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/mx5dr-aa-probe.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
"${CROSS_COMPILE}gcc" -std=gnu99 -O1 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -marm \
    -o "$work/aa_probe" "$project/tests/adapter/aa_install_probe.c" -Wl,--no-as-needed \
    -L"$stock/jci/lib" -ljcicommon -ljcicommon_util -ljcism_service -ljcids \
    -Wl,-rpath-link,"$stock/jci/lib:$stock/usr/lib:$stock/lib" -ldl -lpthread -lrt -lm -lstdc++ -lgcc_s

# run NAME PATCH_DIR_OR_EMPTY [MODE]; keeps the first trace row in boot.json. PATCH_DIR is the
# directory name under /data_persist that holds libpatch-blmjciaapa.so (empty = no third-party
# patch). MODE is the mx5dr.conf mode token (default SHADOW).
run() {
    name=$1; patch_dir=$2; conf_mode=${3:-SHADOW}
    dp="$work/$name"; mkdir -p "$dp/mx5-aa-dr/logs"
    cp "$MX5DR_ARM_BUILD/libmx5dr.so" "$dp/mx5-aa-dr/libmx5dr.so"
    printf 'mode=%s\nmax_log_bytes=41943040\nmax_log_files=3\nsample_ms=1000\n' "$conf_mode" > "$dp/mx5-aa-dr/mx5dr.conf"
    cp "$work/aa_probe" "$dp/aa_probe"
    preload=/data_persist/mx5-aa-dr/libmx5dr.so
    if [ -n "$patch_dir" ]; then
        mkdir -p "$dp/$patch_dir"; cp "$MX5DR_LIBPATCH" "$dp/$patch_dir/libpatch-blmjciaapa.so"
        preload="$preload:/data_persist/$patch_dir/libpatch-blmjciaapa.so"
    fi
    # The stock tree reaches /data_persist through symlinks; bind only that directory.
    QEMU_SET_ENV="LD_PRELOAD=$preload" env LD_LIBRARY_PATH=/jci/lib:/usr/lib:/lib \
        "$proot" -0 -r "$stock" -b "$dp:/data_persist" -w / -q "$qemu" /data_persist/aa_probe \
        > "$dp/out" 2> "$dp/err" < /dev/null || { echo "FAIL AA install probe $name: probe exit"; cat "$dp/err" >&2; return 1; }
    row="$dp/mx5-aa-dr/logs/trace.0.jsonl"
    [ -s "$row" ] || { echo "FAIL AA install probe $name: no trace row"; return 1; }
    head -1 "$row" > "$dp/boot.json"
}
field() { python3 -c "import json,sys; d=json.load(open(sys.argv[1])); print(eval(sys.argv[2]))" "$2" "$1"; }

run no-patch ""
[ "$(field "d['install']" "$work/no-patch/boot.json")" = ok ] &&
[ "$(field "d['session_hooks']" "$work/no-patch/boot.json")" = observing ] ||
    { echo 'FAIL AA install probe no-patch: expected install=ok, session_hooks=observing'; cat "$work/no-patch/boot.json"; exit 1; }
echo 'PASS AA install probe no third-party patch: install=ok, session hooks observing'

run known-patch oem-aa-mod
[ "$(field "d['install']" "$work/known-patch/boot.json")" = ok ] &&
[ "$(field "d['session_hooks']" "$work/known-patch/boot.json")" = declined_third_party_interposer ] &&
[ "$(field "d['install_diag']['stage']" "$work/known-patch/boot.json")" = 3 ] &&
[ "$(field "d['install_diag']['owner']" "$work/known-patch/boot.json")" = /data_persist/oem-aa-mod/libpatch-blmjciaapa.so ] ||
    { echo 'FAIL AA install probe known-patch: expected install=ok, session hooks declined (stage 3, owner libpatch)'; cat "$work/known-patch/boot.json"; exit 1; }
echo 'PASS AA install probe libpatch at its known path: position hook installed, session observation declined'

run unknown-patch other-shim
[ "$(field "d['install']" "$work/unknown-patch/boot.json")" = next_chain_mismatch_or_lazy_binding ] &&
[ "$(field "d['session_hooks']" "$work/unknown-patch/boot.json")" = none ] &&
[ "$(field "d['install_diag']['stage']" "$work/unknown-patch/boot.json")" = 3 ] ||
    { echo 'FAIL AA install probe unknown-patch: an unrecognised owner of the session slots must stay fail-closed'; cat "$work/unknown-patch/boot.json"; exit 1; }
echo 'PASS AA install probe the same library at an unrecognised path stays fail-closed'

# The opt-in BETA mode with the same real BLM and libpatch (validation/BETA_DECISIONS_2026-10-05.md
# 3.7: a BETA-specific session reader made the installer refuse the whole hook). The boot row must
# show the installed hook, the declined session observation and the BETA opt-in with its
# send-storage fence, and the worker must arm BETA. Boot evidence only; no send is driven here.
run beta-known-patch oem-aa-mod BETA
b="$work/beta-known-patch/boot.json"
[ "$(field "d['install']" "$b")" = ok ] &&
[ "$(field "d['mode']" "$b")" = 5 ] &&
[ "$(field "d['session_hooks']" "$b")" = declined_third_party_interposer ] &&
[ "$(field "d['install_diag']['stage']" "$b")" = 3 ] &&
[ "$(field "d['assist_ready']" "$b")" = False ] &&
[ "$(field "d['beta']['mode']" "$b")" = BETA ] &&
[ "$(field "d['beta']['enabled']" "$b")" = True ] &&
[ "$(field "d['beta']['reason']" "$b")" = adapter_opt_in ] &&
[ "$(field "d['beta']['session_fence']" "$b")" = declined_send_storage_counter ] ||
    { echo 'FAIL AA install probe beta-known-patch: expected install=ok, sessions declined and the BETA opt-in'; cat "$b"; exit 1; }
grep -q '"kind":"beta_state",.*"from":"DISABLED","to":"ARMED","reason":"enabled"' \
    "$work/beta-known-patch/mx5-aa-dr/logs/trace.0.jsonl" ||
    { echo 'FAIL AA install probe beta-known-patch: the worker did not arm BETA'; grep beta_ "$work/beta-known-patch/mx5-aa-dr/logs/trace.0.jsonl"; exit 1; }
echo 'PASS AA install probe BETA mode with libpatch: install=ok, session observation declined, BETA opted in'
