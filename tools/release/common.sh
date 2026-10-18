# shellcheck shell=sh
# Sourced by tools/release/*.sh. POSIX sh + bash-compatible.
# Release work directory: first argument of each stage, else $REL_WORK, else
# the pointer written by setup.sh ($REL_POINTER). Every stage logs to
# WORK/jobs/STAGE.log, records WORK/jobs/STAGE.exit and fails closed.
#
# Environment (documented defaults; nothing machine-specific is built in):
#   REL_POINTER      file holding the current WORK path
#                    (${XDG_STATE_HOME:-$HOME/.local/state}/mazda-aa-dr-release/current)
#   REL_MAX_LOAD     a heavy stage waits while the 1-min load is >= this (4)
#   REL_NICE         nice level of heavy stages (10)
#   MX5DR_STOCK_ROOT private stock firmware tree, read only (required by arm/host/pack/stock)
#   MX5DR_LDS_STOCK  original LDS runtime tree (default: MX5DR_STOCK_ROOT)
#   MX5DR_AA_STOCK   stock root for the AA install probe (default: MX5DR_STOCK_ROOT)
#   MX5DR_LIBPATCH   published libpatch-blmjciaapa.so 0.10.0 (G4 probe)
#   MX5DR_LIBPATCH_091 published libpatch-blmjciaapa.so 0.9.1 (G4, vehicle version)
#   MX5DR_LIBPATCH_DIR oem-aa-mod folder (0.9.1) for the replica gates
#   MX5DR_PROOT      proot (proot on PATH); QEMU_ARM qemu-arm (qemu-arm on PATH)
#   MX5_TOOLCHAIN    an existing pinned toolchain to copy (else fetch_m3_toolchain.py)
#   HOST_DBUS_FLAGS / HOST_DBUS_LIBS  D-Bus headers/library for make test (pkg-config default)
set -u
REL_REPO_DEFAULT='junoh-moon/mazda-aa-dr'
REL_POINTER=${REL_POINTER:-${XDG_STATE_HOME:-$HOME/.local/state}/mazda-aa-dr-release/current}
REL_MAX_LOAD=${REL_MAX_LOAD:-4}
REL_NICE=${REL_NICE:-10}
REL_TOOLS=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

die() { echo "FAIL: $*" >&2; exit 1; }

rel_work() {  # resolve WORK from $1, $REL_WORK or the pointer
    if [ -n "${1:-}" ]; then WORK=$1
    elif [ -n "${REL_WORK:-}" ]; then WORK=$REL_WORK
    elif [ -s "$REL_POINTER" ]; then WORK=$(cat "$REL_POINTER")
    else die 'no release work directory (run setup.sh, pass WORK or set REL_WORK)'; fi
    [ -s "$WORK/release.env" ] || die "$WORK/release.env missing (run setup.sh)"
    # shellcheck disable=SC1091
    . "$WORK/release.env"
    SRC=$WORK/source
    # Gate scripts: the pinned commit's own tests/gates; for an older commit
    # without them, the copy next to these release scripts (stock/zip only).
    GATES=$SRC/tests/gates
    [ -d "$GATES" ] || GATES=$(CDPATH= cd -- "$REL_TOOLS/../../tests/gates" && pwd)
    mkdir -p "$WORK/jobs" "$WORK/evidence"
}

# stage_begin NAME: tee everything below into WORK/jobs/NAME.log; record the
# exit status at exit (EXIT trap), so a killed or failed stage never looks done.
stage_begin() {
    STAGE=$1
    rm -f "$WORK/jobs/$STAGE.exit"
    echo "started $(date -u +%Y-%m-%dT%H:%M:%SZ) pid $$" > "$WORK/jobs/$STAGE.start"
    exec > "$WORK/jobs/$STAGE.log" 2>&1
    trap 'rc=$?; echo "$rc" > "$WORK/jobs/$STAGE.exit"; echo "== stage $STAGE exit $rc $(date -u +%H:%M:%SZ)"' EXIT
    echo "== stage $STAGE tag $TAG commit $COMMIT work $WORK $(date -u +%Y-%m-%dT%H:%M:%SZ)"
}

wait_load() {
    while [ "$(cut -d. -f1 /proc/loadavg)" -ge "$REL_MAX_LOAD" ]; do
        echo "waiting: load $(cut -d' ' -f1-3 /proc/loadavg) >= $REL_MAX_LOAD"; sleep 30
    done
}

need() { [ -n "$(eval "echo \${$1:-}")" ] || die "$1 is required"; }

toolchain_env() {
    MX5_TC=$SRC/tools/m3-toolchain
    [ -x "$MX5_TC/bin/arm-cortexa9_neon-linux-gnueabi-g++" ] || die "toolchain missing at $MX5_TC (run build.sh)"
    CROSS_COMPILE=$MX5_TC/bin/arm-cortexa9_neon-linux-gnueabi-
    QEMU_SYSROOT=$MX5_TC/arm-cortexa9_neon-linux-gnueabi/sysroot
    export CROSS_COMPILE QEMU_SYSROOT
}

qemu_env() {  # qemu-arm on PATH for scripts that call it by name
    q=${QEMU_ARM:-$(command -v qemu-arm || true)}
    [ -n "$q" ] && [ -x "$q" ] || die 'qemu-arm not found (QEMU_ARM)'
    QEMU_ARM=$q; export QEMU_ARM
    if [ "$(basename "$q")" != qemu-arm ] || ! command -v qemu-arm >/dev/null 2>&1; then
        mkdir -p "$WORK/qbin" && ln -sf "$q" "$WORK/qbin/qemu-arm"; PATH=$WORK/qbin:$PATH; export PATH
    fi
}

evidence() {  # evidence NAME: list a file in WORK/evidence/INDEX with its sha256
    sha256sum "$WORK/evidence/$1" | sed "s#  .*/#  #" >> "$WORK/evidence/INDEX"
}
