#!/bin/sh
# Touch-only front end for the USB stick: no keyboard and no typed text.
# The CMU update scanner starts this file through the .up entry when the USB
# is inserted; a mounted USB root can run it by hand as well. Every choice is a
# jci-dialog button. Each action feeds the numeric trial menu's own lines to
# its stdin, so the touch route and the shell route run one installer and one
# status and export code path. Nothing changes before an explicit button press.
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

# Blocking dialog with two buttons: 0 first label, 1 second label, any other
# status is a dialog failure. Callers act only on the code they expect.
ask_two() {
    dismiss_popups
    answer=0
    "$DIALOG" --confirm --title="$TITLE" --text="$1" --ok-label="$2" \
        --cancel-label="$3" >/dev/null 2>&1 || answer=$?
    return "$answer"
}

# Blocking dialog with three buttons: 0, 1 and 2 for the first, second and third label.
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
        ask_two 'Installed.\n\nRestart the CMU to start BETA.\nKeep the engine running.' RESTART LATER || answer=$?
        if [ "$answer" = 0 ]; then do_restart; fi
    else
        ask_two 'Install did not finish.\n\nRead touch-log.txt on the USB.\nNo restart was requested.' OK CLOSE || :
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
        ask_two "Check: $verdict\n\nNot GO: keep the USB, then use MORE and SAVE LOGS." OK CLOSE || :
    fi
}

do_remove() {
    answer=0
    ask_two 'Remove MX5 AA DR?\n\nLogs stay on the USB.\nStock returns after a restart.' REMOVE CANCEL || answer=$?
    [ "$answer" = 0 ] || return 0
    show_progress 'Removing MX5 AA DR.\n\nKeep the engine running and the USB connected.'
    rc=0; run_trial 4 || rc=$?
    if [ "$rc" != 0 ]; then
        ask_two 'Removal did not finish.\n\nRead touch-log.txt on the USB.' OK CLOSE || :
        return 0
    fi
    answer=0
    ask_two 'Removed.\n\nRestart the CMU to return to stock.\nKeep the engine running.' RESTART LATER || answer=$?
    if [ "$answer" = 0 ]; then do_restart; fi
}

do_save() {
    answer=0
    ask_two 'Save logs to the USB?\n\nThis stops the product for this boot.\nThe next boot starts it again.' SAVE CANCEL || answer=$?
    [ "$answer" = 0 ] || return 0
    show_progress 'Saving logs.\n\nKeep the USB connected until done.'
    rc=0; run_trial 3 || rc=$?
    if [ "$rc" = 0 ]; then
        ask_two 'Logs saved to the USB.\n\nSee trial-result.txt.' OK CLOSE || :
    else
        ask_two 'Saving logs did not finish.\n\nRead touch-log.txt on the USB.' OK CLOSE || :
    fi
}

do_delete() {
    answer=0
    ask_two 'Delete MX5 AA DR completely?\n\nLogs are deleted too.\nRemove and restart first.' DELETE CANCEL || answer=$?
    [ "$answer" = 0 ] || return 0
    answer=0
    ask_two 'Really delete everything?\n\nThis cannot be undone.' DELETE CANCEL || answer=$?
    [ "$answer" = 0 ] || return 0
    show_progress 'Deleting MX5 AA DR.\n\nKeep the USB connected.'
    rc=0; run_trial 6 6 || rc=$?
    if [ "$rc" = 0 ]; then
        ask_two 'Deleted.\n\nThe USB keeps the installer.' OK CLOSE || :
    else
        ask_two 'Delete did not run.\n\nIt refuses while MX5 AA DR is installed.\nRead touch-log.txt on the USB.' OK CLOSE || :
    fi
}

do_more() {
    answer=0
    ask_three 'More actions.\n\nSAVE LOGS: copy logs to the USB.\nRESTART: restart the CMU.\nDELETE ALL: remove everything.' \
        'SAVE LOGS' RESTART 'DELETE ALL' || answer=$?
    case "$answer" in
        0) do_save ;;
        1)
            answer=0
            ask_two 'Restart the CMU now?\n\nKeep the engine running.' RESTART CANCEL || answer=$?
            if [ "$answer" = 0 ]; then do_restart; fi
            ;;
        2) do_delete ;;
    esac
}

# Fresh USB state: the install lock and the installed marker decide the menu.
menu_fresh() {
    answer=0
    ask_two 'Install MX5 AA DR (BETA)?\n\nPark, engine running, USB connected.\nDo not press the start button.' INSTALL CANCEL || answer=$?
    if [ "$answer" = 0 ]; then do_install; else log_line 'install cancelled; nothing changed'; fi
}

menu_installed() {
    answer=0
    ask_three 'MX5 AA DR is installed.\n\nChoose an action.' CHECK REMOVE MORE || answer=$?
    case "$answer" in
        0) do_check ;;
        1) do_remove ;;
        2) do_more ;;
        *) log_line 'dialog closed without a choice; nothing changed' ;;
    esac
}

log_line "touch: started (default route, no keyboard)"
if [ -f "$BASE/installed.txt" ] && [ ! -L "$BASE/installed.txt" ]; then
    menu_installed
else
    menu_fresh
fi
rm -f "$OUT" 2>/dev/null || :
sync 2>/dev/null || :
