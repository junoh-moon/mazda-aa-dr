#!/bin/sh
# Called only after the numeric menu's explicit reboot choice while parked.
# A saved request or a zero command exit does not prove a completed new boot.
HERE=$(CDPATH= cd -P -- "$(dirname -- "$0")" && pwd -P) || exit 1
. "$HERE/common.sh"
[ "$#" = 0 ] || fail 'Usage: sh reboot_cmu.sh (explicit parked reboot request)'

reboot_program=$ROOT/sbin/reboot
if [ -n "$ROOT" ]; then
    # Never fall back to the host command when a marked fixture lacks its
    # explicit executable witness. Production BusyBox's reboot symlink is OK.
    [ -d "$ROOT/sbin" ] && [ ! -L "$ROOT/sbin" ] &&
        [ -f "$reboot_program" ] && [ ! -L "$reboot_program" ] && [ -x "$reboot_program" ] ||
        fail 'Missing regular executable fixture reboot witness'
else
    [ -x "$reboot_program" ] || fail 'Original /sbin/reboot is unavailable'
fi

require_usb() {
    # /proc/mounts is normally the kernel's self/mounts symlink. The last row
    # for this exact target is effective; an older USB row cannot override a
    # later mount. A leftover directory on CMU tmpfs is not a USB destination.
    [ -f "$ROOT/proc/mounts" ] && [ -r "$ROOT/proc/mounts" ] ||
        fail 'Cannot inspect the mounted USB'
    awk -v dest="$HERE" '$2==dest {found=(NF==6 && $1 ~ /^\/dev\/sd[a-z]+[0-9]+$/)}
        END {exit !found}' "$ROOT/proc/mounts" ||
        fail 'Run reboot from the mounted USB root; keep that USB connected.'
    # The subshell holds its actual USB working directory across the write.
    # If detached later, relative writes retain that filesystem or fail; they
    # cannot switch to a newly exposed CMU tmpfs directory at the same path.
    current_directory=$(stat -c '%d:%i' .) || fail 'Cannot identify the USB directory'
    named_directory=$(stat -c '%d:%i' "$HERE") || fail 'Cannot identify the mounted USB'
    [ "$current_directory" = "$named_directory" ] || fail 'The mounted USB changed'
}

# Bound proc reads even in a malformed fixture; missing diagnostics do not veto
# the explicit normal reboot request. The receipt never contains raw proc text.
read_small() {
    small=''
    [ -f "$1" ] && [ -r "$1" ] || return 1
    small=$(dd if="$1" bs=1 count="$(($2 + 1))" 2>/dev/null || exit 1; printf '.') || return 1
    small=${small%.}
    [ "${#small}" -le "$2" ] || return 1
    small_size=$(stat -c %s "$1" 2>/dev/null) || return 1
    case "$small_size" in ''|*[!0-9]*) return 1;; esac
    # Kernel proc files normally have size zero. Ordinary fixtures must agree
    # with the byte count too, so shell removal of an embedded NUL is rejected.
    [ "$small_size" = 0 ] || [ "$small_size" = "${#small}" ]
}
boot_id=unavailable; boot_id_status=unavailable
if read_small "$ROOT/proc/sys/kernel/random/boot_id" 37 && [ "${#small}" = 37 ]; then
    candidate=${small%?}
    if [ "$small" = "$candidate
" ] && valid_boot_id "$candidate"; then
        boot_id=$candidate; boot_id_status=valid
    fi
fi
uptime_seconds=unavailable; uptime_status=unavailable
if read_small "$ROOT/proc/uptime" 128; then
    if candidate=$(printf '%s' "$small" | awk '
        NR==1 && NF==2 && length($1)<=24 && $1 ~ /^[0-9]+([.][0-9]+)?$/ && $2 ~ /^[0-9]+([.][0-9]+)?$/ {value=$1;ok=1}
        END {if(NR!=1 || !ok)exit 1;print value}'); then
        uptime_seconds=$candidate; uptime_status=valid
    fi
fi

# Keep common.sh's remount state and EXIT cleanup inside this transaction. The
# parent invokes reboot only after sync and any original read-only restoration
# finish successfully; its own common cleanup has no pending remounts.
(
    receipt_tmp=reboot-request.txt.tmp.$$
    receipt_owned=0
    receipt_cleanup() {
        receipt_rc=$?
        trap - 0 HUP INT TERM
        set +e
        if [ "$receipt_owned" = 1 ]; then rm -f "$receipt_tmp" || receipt_rc=1; fi
        (exit "$receipt_rc")
        cleanup
    }
    trap receipt_cleanup 0
    trap 'exit 1' HUP INT TERM
    cd -P -- "$HERE" || fail 'Cannot enter the USB root'
    require_usb
    ALLOW_REMOUNT=1
    mount_rw "$HERE"
    require_usb
    receipt=reboot-request.txt
    if [ -e "$receipt" ] || [ -L "$receipt" ]; then regular "$receipt"; fi
    umask 077
    set -C
    exec 8> "$receipt_tmp"
    set +C
    receipt_owned=1
    printf 'reboot_schema=1\nrequest=normal\ncompletion=unconfirmed\nboot_id_status=%s\nboot_id=%s\nuptime_status=%s\nuptime_seconds=%s\n' \
        "$boot_id_status" "$boot_id" "$uptime_status" "$uptime_seconds" >&8 || fail 'Cannot write the USB reboot receipt'
    exec 8>&-
    require_usb
    mv -f "$receipt_tmp" "$receipt" || fail 'Cannot save the USB reboot receipt'
    receipt_owned=0
    sync
) || exit $?

printf 'reboot_receipt=reboot-request.txt\nreboot_request=normal\nreboot_completion=unconfirmed\n'
# No -f, shell reset, direct syscall, or fallback command. Normal BusyBox reboot
# requests the existing PID 1 shutdown path; return 0 is only request acceptance.
if "$reboot_program"; then reboot_rc=0; else reboot_rc=$?; fi
printf 'reboot_command_exit=%s\n' "$reboot_rc" || :
exit "$reboot_rc"
