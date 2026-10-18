#!/bin/sh
# Release stage 6 (G7, G8, G9, G16): the tracked stock-BusyBox gates of
# tests/gates on the unpacked release bundle. Each gate writes
# WORK/evidence/stock-NAME.txt; WORK/evidence/stock-summary.txt lists every
# gate with its exit code. A skip (exit 77) fails the stage unless the gate
# is named in REL_ALLOW_SKIP (space separated), e.g. when the private vehicle
# export is not on this machine.
#   sh tools/release/stock.sh [WORK]
# Inputs: MX5DR_STOCK_ROOT, MX5DR_PROOT, QEMU_ARM, MX5DR_LIBPATCH_DIR
# (oem-aa-mod 0.9.1 folder), MX5DR_VEHICLE_EXPORT (private, touch/menu-6 gates),
# REL_PREVIOUS_TAG and REL_PREVIOUS_SHA256 (previous public ZIP for G8/G16,
# downloaded with gh), REL_G9_STRIDE (1 = every cut point), REL_GATE_FLAGS
# (extra G8 flags, e.g. '--exp-markers present --trace-cleanup yes'),
# MX5DR_GATE_TOGGLES (G7 menu list of the candidate).
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
rel_work "${1:-}"
stage_begin stock
need MX5DR_STOCK_ROOT; need REL_PREVIOUS_TAG; need REL_PREVIOUS_SHA256
qemu_env
cd "$SRC" || exit 1
export MX5DR_GATE_NICE=19
prev=$WORK/previous/mazda-aa-dr-$REL_PREVIOUS_TAG.zip
if [ ! -s "$prev" ]; then
    mkdir -p "$WORK/previous"
    gh release download "$REL_PREVIOUS_TAG" --repo "$REPO" --pattern "mazda-aa-dr-$REL_PREVIOUS_TAG.zip" --dir "$WORK/previous" || exit 1
fi
echo "$REL_PREVIOUS_SHA256  $prev" | sha256sum -c - || exit 1
cand=$WORK/unpacked
summary=$WORK/evidence/stock-summary.txt
: > "$summary"
rc=0
gate() {  # gate NAME CMD...
    name=$1; shift
    wait_load
    echo "== $name start $(date -u +%H:%M:%SZ)"
    MX5DR_GATE_WORK=$WORK/gates/$name "$@" > "$WORK/evidence/stock-$name.txt" 2>&1
    e=$?
    echo "$name exit=$e $(grep -E '^SUMMARY|^G[0-9]+:|PASS|^SKIP' "$WORK/evidence/stock-$name.txt" | tail -1)" | tee -a "$summary"
    evidence "stock-$name.txt"
    if [ "$e" = 77 ]; then
        case " ${REL_ALLOW_SKIP:-} " in *" $name "*) ;; *) rc=1;; esac
    elif [ "$e" != 0 ]; then rc=1; fi
}
gate g7-cmu-emulation python3 "$GATES"/g7_cmu_emulation.py --bundle "$cand"
gate g7-multiboot python3 "$GATES"/g7_multiboot.py --bundle "$cand"
gate g7-menus python3 "$GATES"/g7_menus.py --bundle "$cand"
gate g7-touch-A python3 "$GATES"/g7_touch.py --bundle "$cand" --scenario A
gate g7-touch-B python3 "$GATES"/g7_touch.py --bundle "$cand" --scenario B
if [ -n "${MX5DR_VEHICLE_EXPORT:-}" ] && [ -n "${MX5DR_LIBPATCH_DIR:-}" ]; then
    gate g7-menu6-replica nice -n 19 python3 tests/packaging/replica_clean_uninstall.py --stock "$MX5DR_STOCK_ROOT" \
        --bundle "$cand" --work "$WORK/gates/menu6" --proot "${MX5DR_PROOT:-proot}" --qemu "$QEMU_ARM" \
        --vehicle "$MX5DR_VEHICLE_EXPORT" --libpatch "$MX5DR_LIBPATCH_DIR"
else
    echo 'g7-menu6-replica exit=77 SKIP: MX5DR_VEHICLE_EXPORT/MX5DR_LIBPATCH_DIR unset' | tee -a "$summary"
    case " ${REL_ALLOW_SKIP:-} " in *" g7-menu6-replica "*) ;; *) rc=1;; esac
fi
# shellcheck disable=SC2086
gate g8-upgrade python3 "$GATES"/g8_upgrade.py --previous "$prev" --previous-sha256 "$REL_PREVIOUS_SHA256" \
    --candidate "$cand" ${REL_GATE_FLAGS:-}
gate g16-rollback python3 "$GATES"/g16_rollback.py --candidate "$cand" --previous "$prev" \
    --previous-sha256 "$REL_PREVIOUS_SHA256"
gate g9-powerloss python3 "$GATES"/g9_powerloss.py --candidate "$cand" --stride "${REL_G9_STRIDE:-1}"
cat "$summary"
evidence stock-summary.txt
exit $rc
