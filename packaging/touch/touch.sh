#!/bin/sh
# Touch-only front end for the USB stick: no keyboard and no typed text.
# The CMU update scanner starts this file through the .up entry when the USB
# is inserted; a mounted USB root can run it by hand as well. Every choice is a
# jci-dialog button. Each action feeds the numeric trial menu's own lines to
# its stdin, so the touch route and the shell route run one installer and one
# status and export code path. Nothing changes before an explicit button press.
#
# Three stick states drive the menu:
#   INSTALLED  installed.txt present      -> check / save logs / reinstall / remove
#   RESIDUE    package dir but no marker  -> save logs / delete files / reinstall
#   FRESH      neither                    -> install
# Full removal spans a mandatory reboot (the firmware refuses to delete the
# files while the collector and preloaded services are still live), so REMOVE
# guides the user to restart and the RESIDUE state then offers DELETE FILES.
HERE=$(CDPATH= cd -P -- "$(dirname -- "$0")" && pwd -P) || exit 1
. "$HERE/common.sh"
[ "$#" = 0 ] || fail 'Usage: touch.sh (started by the USB entry; no arguments)'

TITLE='MX5 AA DR'
DIALOG=$ROOT/jci/tools/jci-dialog
LOG=$HERE/touch-log.txt
OUT=$ROOT/tmp/mx5dr-touch.$$.out
answer=0
rc=0
verdict=''

# common.sh's own EXIT trap restores the mounts it changed; this script adds
# none. Writing the log needs the stick writable; a fixture root never remounts.
[ -n "$ROOT" ] || mount -o rw,remount "$HERE" 2>/dev/null || :

log_line() { printf '%s\n' "$1" >> "$LOG" 2>/dev/null || :; }
log_block() { { printf '== %s\n' "$1"; cat "$2"; } >> "$LOG" 2>/dev/null || :; }

[ -x "$DIALOG" ] || { log_line 'touch: jci-dialog is missing; nothing changed'; exit 1; }

dismiss_popups() { killall -q jci-dialog 2>/dev/null || :; }

# Non-blocking progress text. The next dialog replaces it.
show_progress() {
    dismiss_popups
    "$DIALOG" --info --title="$TITLE" --text="$1" --no-cancel >/dev/null 2>&1 &
}

# Blocking dialog with two buttons. The left button returns 0 and is always the
# safe choice: a stray tap in the left half is the likely accident, so every
# destructive action sits on the right and returns 1. Any other status is a
# dialog failure; callers act only on the code they expect.
ask_two() {
    dismiss_popups
    answer=0
    "$DIALOG" --confirm --title="$TITLE" --text="$1" --ok-label="$2" \
        --cancel-label="$3" >/dev/null 2>&1 || answer=$?
    return "$answer"
}

# Blocking three-button dialog (0, 1, 2 left to right). These only navigate to a
# two-button confirm; no destructive action commits straight from here, so a
# stray tap picks a submenu at worst. Callers act only on 0/1/2.
ask_three() {
    dismiss_popups
    answer=0
    "$DIALOG" --3-button-dialog --title="$TITLE" --text="$1" --ok-label="$2" \
        --cancel-label="$3" --button3-label="$4" >/dev/null 2>&1 || answer=$?
    return "$answer"
}

# Feed one trial menu line per argument. The trial exits nonzero when its own
# step failed; its printed output is kept on the stick for diagnosis.
run_trial() {
    trial_rc=0
    printf '%s\n' "$@" | sh "$HERE/trial" > "$OUT" 2>&1 || trial_rc=$?
    log_block "trial $* exit=$trial_rc" "$OUT"
    return "$trial_rc"
}

do_restart() {
    show_progress 'Restarting the CMU.\n\nKeep the engine running.'
    rc=0; run_trial 5 || rc=$?
    if [ "$rc" != 0 ]; then
        ask_two 'Restart was not requested.\n\nRead touch-log.txt on the USB.' OK CLOSE || :
    fi
}

do_install() {
    show_progress 'Installing MX5 AA DR.\n\nKeep the engine running and the USB connected.'
    rc=0; run_trial 1 0 || rc=$?
    if [ "$rc" = 0 ] && grep -q '^Installation finished' "$OUT"; then
        answer=0
        ask_two 'Installed.\n\nRestart the CMU to start BETA.\nKeep the engine running.' LATER RESTART || answer=$?
        if [ "$answer" = 1 ]; then do_restart; fi
    else
        ask_two 'Install did not finish.\n\nRead touch-log.txt on the USB.\nNo restart was requested.' OK CLOSE || :
    fi
}

# Re-enable after a trip, update to this stick's bundle, or recover an
# interrupted install. The trial's menu 1 is idempotent and resets experimental
# markers to the defaults, exactly as the shell route.
do_reinstall() {
    answer=0
    ask_two 'Reinstall / re-enable MX5 AA DR?\n\nUse this after a trip or to update.\nExperimental options reset to default.' CANCEL REINSTALL || answer=$?
    [ "$answer" = 1 ] || return 0
    show_progress 'Reinstalling MX5 AA DR.\n\nKeep the engine running and the USB connected.'
    rc=0; run_trial 1 || rc=$?
    if [ "$rc" = 0 ] && grep -q '^Installation finished' "$OUT"; then
        answer=0
        ask_two 'Reinstalled.\n\nRestart the CMU to start BETA.\nKeep the engine running.' LATER RESTART || answer=$?
        if [ "$answer" = 1 ]; then do_restart; fi
    else
        ask_two 'Reinstall did not finish.\n\nRead touch-log.txt on the USB.\nNo restart was requested.' OK CLOSE || :
    fi
}

do_check() {
    show_progress 'Checking the last boot.\n\nKeep the engine running.'
    rc=0; run_trial 2 0 || rc=$?
    verdict=$(grep -E '^(GO|NO-GO.*|WAIT 60 s, then run 2 again)$' "$OUT" | tail -n 1) || :
    if [ "$rc" != 0 ] || [ -z "$verdict" ]; then
        ask_two 'No verdict.\n\nRead touch-log.txt on the USB.' OK CLOSE || :
    elif [ "$verdict" = GO ]; then
        ask_two 'Check: GO\n\nDetails: startup-result.txt on the USB.' OK CLOSE || :
    else
        ask_two "Check: $verdict\n\nNot GO: SAVE LOGS, then REINSTALL to re-enable." OK CLOSE || :
    fi
}

do_save() {
    answer=0
    ask_two 'Save logs to the USB?\n\nThis stops the product for this boot.\nThe next boot starts it again.' CANCEL SAVE || answer=$?
    [ "$answer" = 1 ] || return 0
    show_progress 'Saving logs.\n\nKeep the USB connected until done.'
    rc=0; run_trial 3 || rc=$?
    if [ "$rc" = 0 ]; then
        ask_two 'Logs saved to the USB.\n\nSee trial-result.txt.' OK CLOSE || :
    else
        ask_two 'Saving logs did not finish.\n\nRead touch-log.txt on the USB.' OK CLOSE || :
    fi
}

# Step 1 of full removal: uninstall, then guide to the mandatory restart. After
# the restart the stick is in RESIDUE and offers DELETE FILES to finish.
do_remove() {
    answer=0
    ask_two 'Remove MX5 AA DR? (step 1 of 2)\n\nLogs are kept. Stock returns after a restart.\nThen re-insert the USB to delete files.' CANCEL REMOVE || answer=$?
    [ "$answer" = 1 ] || return 0
    show_progress 'Removing MX5 AA DR.\n\nKeep the engine running and the USB connected.'
    rc=0; run_trial 4 || rc=$?
    if [ "$rc" != 0 ]; then
        ask_two 'Removal did not finish.\n\nRead touch-log.txt on the USB.' OK CLOSE || :
        return 0
    fi
    answer=0
    ask_two 'Removed. (step 2)\n\nRestart, then re-insert the USB to delete files\nor leave them for logs.' LATER RESTART || answer=$?
    if [ "$answer" = 1 ]; then do_restart; fi
}

# Final deletion, reachable only in RESIDUE. The firmware refuses while the
# product is still live, so detect that and send the user back to restart.
do_finish_delete() {
    answer=0
    ask_two 'Delete all MX5 AA DR files?\n\nLogs and backups go too. Save logs first if\nyou still need them.' CANCEL DELETE || answer=$?
    [ "$answer" = 1 ] || return 0
    answer=0
    ask_two 'Really delete everything?\n\nThis cannot be undone.' CANCEL DELETE || answer=$?
    [ "$answer" = 1 ] || return 0
    show_progress 'Deleting MX5 AA DR.\n\nKeep the USB connected.'
    rc=0; run_trial 6 6 || rc=$?
    if [ "$rc" = 0 ]; then
        ask_two 'Deleted.\n\nThe USB keeps the installer.' OK CLOSE || :
    elif grep -qE 'run 4, then 5|reboot with 5|collector is running|install lock' "$OUT"; then
        answer=0
        ask_two 'Not yet deleted.\n\nThe product is still live. Restart first,\nthen re-insert and delete.' LATER RESTART || answer=$?
        if [ "$answer" = 1 ]; then do_restart; fi
    else
        ask_two 'Delete did not run.\n\nRead touch-log.txt on the USB.' OK CLOSE || :
    fi
}

do_restart_confirm() {
    answer=0
    ask_two 'Restart the CMU now?\n\nKeep the engine running.' CANCEL RESTART || answer=$?
    if [ "$answer" = 1 ]; then do_restart; fi
}

# FRESH: a bare install prompt.
menu_fresh() {
    answer=0
    ask_two 'Install MX5 AA DR (BETA)?\n\nPark, engine running, USB connected.\nDo not press the start button.' CANCEL INSTALL || answer=$?
    if [ "$answer" = 1 ]; then do_install; else log_line 'install cancelled; nothing changed'; fi
}

# INSTALLED: check / save logs, with manage (reinstall / restart / remove) under MORE.
menu_installed() {
    answer=0
    ask_three 'MX5 AA DR is installed.\n\nChoose an action.' CHECK 'SAVE LOGS' MORE || answer=$?
    case "$answer" in
        0) do_check ;;
        1) do_save ;;
        2)
            answer=0
            ask_three 'Manage MX5 AA DR.\n\nREINSTALL: re-enable or update.\nRESTART: restart the CMU.\nREMOVE: uninstall (step 1 of delete).' \
                REINSTALL RESTART REMOVE || answer=$?
            case "$answer" in
                0) do_reinstall ;;
                1) do_restart_confirm ;;
                2) do_remove ;;
            esac
            ;;
        *) log_line 'dialog closed without a choice; nothing changed' ;;
    esac
}

# RESIDUE: uninstalled, files remain. Offer the finish-delete and recovery paths.
menu_residue() {
    answer=0
    ask_three 'MX5 AA DR is removed; files remain.\n\nSAVE LOGS before deleting if needed.\nDELETE FILES finishes removal.' \
        'SAVE LOGS' REINSTALL 'DELETE FILES' || answer=$?
    case "$answer" in
        0) do_save ;;
        1) do_reinstall ;;
        2) do_finish_delete ;;
        *) log_line 'dialog closed without a choice; nothing changed' ;;
    esac
}

log_line "touch: started (default route, no keyboard)"
# RESIDUE means real leftover files, not just an empty directory: uninstall
# keeps the mapped library and the logs, and purge removes them. An empty or
# absent package directory is FRESH.
if [ -f "$BASE/installed.txt" ] && [ ! -L "$BASE/installed.txt" ]; then
    log_line 'state: installed'
    menu_installed
elif { [ -e "$BASE/libmx5dr.so" ] || [ -d "$BASE/logs" ]; } && [ ! -L "$BASE" ]; then
    log_line 'state: residue (removed, files remain)'
    menu_residue
else
    log_line 'state: fresh'
    menu_fresh
fi
rm -f "$OUT" 2>/dev/null || :
sync 2>/dev/null || :
