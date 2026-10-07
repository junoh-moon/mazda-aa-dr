#!/bin/sh
# Menu 6: delete everything this package left on the CMU, after menu 4 and a
# new CMU boot (menu 5). Read-only preconditions come first; any doubt refuses
# with one line and an exit code, and deletes nothing. OEM files (/jci, the
# autostart content, /data reports) and other tools' files (oem-aa-mod, the
# touch mod) are never written, restored or removed.
#   3 package still installed   4 package file in use or unprovable
#   5 collector running          6 install lock held
#   7 unexpected file type        8 file system mounted inside it
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/common.sh"
[ "$#" = 1 ] || fail 'Usage: sh purge.sh /mounted/usb/directory'
usb=$1
case "$usb" in /*) ;; *) fail 'USB directory must be absolute';; esac
[ -d "$usb" ] && [ ! -L "$usb" ] || fail 'USB directory is not a real directory'
usb=$(CDPATH= cd -P -- "$usb" && pwd -P) || fail 'Cannot resolve the USB directory'
ALLOW_REMOUNT=1
[ -z "$ROOT" ] || ALLOW_REMOUNT=0

refuse() {
    echo "Refused, nothing deleted: $2"
    exit "$1"
}
if [ -z "$ROOT" ]; then
    awk -v p="$usb" '$2=="/" || p==$2 || index(p,$2"/")==1 {
        if(length($2)>=n){n=length($2);device=$1}}
        END{exit !(device ~ /^\/dev\/sd[a-z]+[0-9]+$/)}' /proc/mounts ||
        fail 'Run from the mounted USB; keep that USB connected.'
    # Every installer, rearm, uninstall and export holds this kernel lock for
    # as long as it holds the install lock directory. Take it before any check.
    lock_mounts
fi
validate_persist
[ ! -L "$BASE" ] || refuse 7 'the package directory is a symlink'
AUTOSTART=$ROOT/usr/bin/autostart
CONFIGS="$ROOT/jci/sm/sm.conf $ROOT/jci/sm/sm_WCP.conf"

# --- 1. Read-only preconditions -------------------------------------------
check_uninstalled() {
    for marker in arm persist; do
        if [ -e "$BASE/guard/$marker" ] || [ -L "$BASE/guard/$marker" ]; then
            refuse 3 "guard/$marker present; run 4, then 5, first"
        fi
    done
    [ -f "$AUTOSTART" ] && [ ! -L "$AUTOSTART" ] || refuse 7 'cannot read /usr/bin/autostart'
    if grep -F -e 'MX5DR ONE-BOOT' -e '/mx5-aa-dr/' "$AUTOSTART" >/dev/null; then
        refuse 3 'autostart has mx5dr blocks; run 4, then 5'
    else
        [ "$?" = 1 ] || refuse 7 'cannot read /usr/bin/autostart'
    fi
    for config in $CONFIGS; do
        [ -e "$config" ] || [ -L "$config" ] || continue
        [ -f "$config" ] && [ ! -L "$config" ] || refuse 7 "unexpected file type: ${config#"$ROOT"}"
        if grep -F -e 'libmx5dr' -e '/mx5-aa-dr/' "$config" >/dev/null; then
            refuse 3 "${config##*/} has mx5dr tokens; run 4, then 5"
        else
            [ "$?" = 1 ] || refuse 7 "cannot read ${config#"$ROOT"}"
        fi
    done
}

# Kernel-owned truth only: memory maps, open descriptors and command lines of
# every process. A process whose entries cannot be read cannot be proven
# unrelated, so it refuses; a process that exited meanwhile is skipped.
in_use() {
    case "$1" in
        *'/data_persist/mx5-aa-dr/'*|*'/data_persist/mx5-aa-dr '*|*'/data_persist/mx5-aa-dr'|*'/data_persist/mx5-aa-dr
'*|*'/tmp/mx5dr-trial-'*|*'/tmp/mx5dr-hash.'*|*'/tmp/mx5-lds-association-'*) return 0;;
    esac
    return 1
}
check_processes() {
    scanned=0
    for proc in "$ROOT"/proc/[0-9]*; do
        pid=${proc##*/}
        case "$pid" in ''|*[!0-9]*) continue;; esac
        [ "$pid" != "$$" ] || continue
        [ -d "$proc" ] && [ ! -L "$proc" ] || continue
        for entry in maps fd cmdline; do
            case "$entry" in
                maps) seen=$(cat "$proc/maps" 2>/dev/null) && read_ok=1 || read_ok=0;;
                fd) seen=$(ls -l "$proc/fd/" 2>/dev/null) && read_ok=1 || read_ok=0;;
                cmdline) seen=$(tr '\000' ' ' < "$proc/cmdline" 2>/dev/null) && read_ok=1 || read_ok=0;;
            esac
            if [ "$read_ok" = 0 ]; then
                [ -d "$proc" ] || continue 2
                refuse 4 "cannot read /proc/$pid/$entry; cannot prove it is unused"
            fi
            if in_use "$seen"; then
                refuse 4 "process $pid uses a package file ($entry); reboot with 5 first"
            fi
        done
        scanned=$((scanned + 1))
    done
    # An empty or missing process table proves nothing.
    [ "$scanned" -gt 0 ] || refuse 4 'no readable /proc process entries'
}
check_collector() {
    collector_lock=$BASE/logs/collector.lock
    if [ -L "$collector_lock" ]; then refuse 7 'collector lock is a symlink'; fi
    [ -e "$collector_lock" ] || return 0
    [ -f "$collector_lock" ] || refuse 7 'collector lock is not a regular file'
    # The collector keeps a kernel flock on this file while it runs. Open it
    # read-only (never create) in a subshell that releases it on exit.
    ( exec 7< "$collector_lock" && flock -n 7 ) 2>/dev/null ||
        refuse 5 'collector is running (its lock is held); reboot with 5 first'
}
# BusyBox 1.19.2 rm has no -xdev: never recurse into another file system.
check_submounts() {
    [ -d "$BASE" ] && [ ! -L "$BASE" ] || return 0
    real_base=$(CDPATH= cd -P -- "$BASE" && pwd -P) || refuse 7 'cannot resolve the package directory'
    mount_table=$(cat "$ROOT/proc/mounts") || refuse 8 'cannot read /proc/mounts'
    if printf '%s\n' "$mount_table" | awk -v b="$real_base" '$2==b || index($2, b "/")==1 {found=1} END {exit !found}'; then
        refuse 8 'a file system is mounted inside the package directory'
    fi
}
STALE_LOCK=no
check_lock() {
    [ -e "$LOCK" ] || [ -L "$LOCK" ] || return 0
    [ -d "$LOCK" ] && [ ! -L "$LOCK" ] || refuse 7 'install lock is not a directory'
    # A live installer, rearm, uninstall or purge keeps the kernel mount lock
    # until it has removed this directory. Holding that lock ourselves proves
    # the directory is left over from an interrupted run (e.g. power loss).
    [ -z "$ROOT" ] && [ "$MOUNT_LOCKED" = 1 ] || refuse 6 'install lock is held; another installer may be active'
    for entry in "$LOCK"/* "$LOCK"/.[!.]*; do
        [ -e "$entry" ] || [ -L "$entry" ] || continue
        [ "$entry" = "$LOCK/pid" ] && [ -f "$entry" ] && [ ! -L "$entry" ] ||
            refuse 6 'install lock holds unexpected entries; inspect it'
    done
    STALE_LOCK=yes
}

# --- 2. Inventory (read-only) ---------------------------------------------
# Files the installer, guard, collector and runtime create outside the package
# directory: interrupted staging copies next to the OEM files (never the OEM
# files themselves), and /tmp work files. /tmp/.mx5dr-mount.lock stays: other
# invocations may hold its inode, and tmpfs drops it at the next boot.
digits() { case "$1" in ''|*[!0-9]*) return 1;; esac; return 0; }
staged_suffix() {
    # <pid> or <pid>.tap.<pid> / <pid>.lds.<pid>
    first=${1%%.*}
    digits "$first" || return 1
    rest=${1#"$first"}
    case "$rest" in
        '') return 0;;
        .tap.*|.lds.*) digits "${rest#.???.}";;
        *) return 1;;
    esac
}
# mkdtemp/mkstemp (glibc) and BusyBox mktemp replace XXXXXX with exactly six
# characters from [A-Za-z0-9]. Anything else is not ours.
random6() {
    [ "${#1}" = 6 ] || return 1
    case "$1" in *[!A-Za-z0-9]*) return 1;; esac
    return 0
}
# Never collect candidates into a word list: each glob result is checked and
# handled here, always quoted, so no name is split or glob-expanded again.
owned_external() {
    name=${1##*/}
    kind=file
    case "$name" in
        autostart.mx5dr-new.*) staged_suffix "${name#autostart.mx5dr-new.}" || return 1;;
        autostart.mx5dr-remove.*) staged_suffix "${name#autostart.mx5dr-remove.}" || return 1;;
        sm.conf.mx5dr-remove.*) staged_suffix "${name#sm.conf.mx5dr-remove.}" || return 1;;
        sm_WCP.conf.mx5dr-remove.*) staged_suffix "${name#sm_WCP.conf.mx5dr-remove.}" || return 1;;
        mx5dr-trial-*) random6 "${name#mx5dr-trial-}" || return 1; kind=trial;;
        mx5dr-hash.*) random6 "${name#mx5dr-hash.}" || return 1; kind=hash;;
        mx5-lds-association-*) random6 "${name#mx5-lds-association-}" || return 1;;
        *) return 1;;
    esac
    case "$kind" in
        file) [ -f "$1" ] && [ ! -L "$1" ] || return 1;;
        trial|hash)
            [ -d "$1" ] && [ ! -L "$1" ] || return 1
            want=sm.conf
            [ "$kind" = trial ] || want=hash
            for inner in "$1"/* "$1"/.[!.]* "$1"/..?*; do
                [ -e "$inner" ] || [ -L "$inner" ] || continue
                [ "$inner" = "$1/$want" ] && [ -f "$inner" ] && [ ! -L "$inner" ] || return 1
            done;;
    esac
    return 0
}
remove_external() {
    case "$1" in
        "$ROOT"/usr/bin/*) mount_rw "$ROOT/usr/bin";;
        "$ROOT"/jci/sm/*) mount_rw "$ROOT/jci/sm";;
    esac
    if [ -d "$1" ] && [ ! -L "$1" ]; then
        rm -f "$1/$want" || fail "Cannot remove ${1#"$ROOT"}/$want"
        rmdir "$1" || fail "Cannot remove ${1#"$ROOT"}"
    else
        rm -f "$1" || fail "Cannot remove ${1#"$ROOT"}"
    fi
}
# $1: count | list | unexpected | delete | remaining
EXTERNAL_COUNT=0
external_pass() {
    for path in "$ROOT"/usr/bin/autostart.mx5dr-* "$ROOT"/jci/sm/sm.conf.mx5dr-* \
                "$ROOT"/jci/sm/sm_WCP.conf.mx5dr-* "$ROOT"/tmp/mx5dr-trial-* \
                "$ROOT"/tmp/mx5dr-hash.* "$ROOT"/tmp/mx5-lds-association-*; do
        [ -e "$path" ] || [ -L "$path" ] || continue
        if owned_external "$path"; then
            case "$1" in
                count) EXTERNAL_COUNT=$((EXTERNAL_COUNT + 1));;
                list)
                    describe "$path"
                    [ "$kind" = file ] || describe "$path/$want";;
                delete) remove_external "$path";;
                remaining) printf ' %s' "${path#"$ROOT"}";;
            esac
        elif [ "$1" = unexpected ]; then
            printf 'unexpected, left as is: %s\n' "${path#"$ROOT"}"
        fi
    done
}
describe() {
    # "<kind> <bytes> <path as on the CMU>"
    if [ -L "$1" ]; then printf 'link 0 %s\n' "${1#"$ROOT"}"
    elif [ -f "$1" ]; then printf 'file %s %s\n' "$(wc -c < "$1")" "${1#"$ROOT"}"
    elif [ -d "$1" ]; then printf 'dir 0 %s\n' "${1#"$ROOT"}"
    else printf 'other 0 %s\n' "${1#"$ROOT"}"; fi
}
inventory() {
    if [ -e "$BASE" ]; then
        # Reporting only; deletion never reads these names back.
        find "$BASE" -print | while IFS= read -r item; do describe "$item"; done
    fi
    external_pass list
}

# --- 3. Pre-install comparison (read-only) ----------------------------------
# install.sh copies autostart, sm.conf and sm_WCP.conf to backups/<set>/*.before
# with their sha256 before it edits anything. The set names use the CMU clock,
# which can restart at 1970 on every boot, so the name order is only nominal:
# report it, and whether all complete records agree.
COMPARE_FILES='autostart sm.conf sm_WCP.conf'
oem_path() {
    case "$1" in autostart) printf '%s\n' "$AUTOSTART";; *) printf '%s\n' "$ROOT/jci/sm/$1";; esac
}
first_field() { awk 'NR==1 {print $1}' "$1" 2>/dev/null || :; }
backup_set_name() {
    # YYYYMMDDTHHMMSS-<pid>, exactly as install.sh names a transaction.
    case "$1" in [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]T[0-9][0-9][0-9][0-9][0-9][0-9]-*) ;; *) return 1;; esac
    digits "${1#*-}"
}
now_of() {
    case "$1" in autostart) printf '%s\n' "$NOW_AUTOSTART";; sm.conf) printf '%s\n' "$NOW_SM";; *) printf '%s\n' "$NOW_WCP";; esac
}
# The current files are compared with EVERY complete record. "identical" is
# claimed only when all records agree and the current files match them; when
# records disagree (e.g. another tool edited a file between two installs and
# the clock restarted, so the name order is not the install order) the report
# names the records and which of them the current files match, never more.
compare_pre_install() {
    BASIS=''; COMPLETE=0; INCOMPLETE=''; AGREE=yes; SETS=''; MATCHING=''
    COMPARE_LINES=''; DIFFERS=''
    for file in $COMPARE_FILES; do
        current=$(oem_path "$file")
        now=missing
        if [ -f "$current" ] && [ ! -L "$current" ]; then now=$(hash "$current") || now=unreadable; fi
        case "$file" in autostart) NOW_AUTOSTART=$now;; sm.conf) NOW_SM=$now;; *) NOW_WCP=$now;; esac
        COMPARE_LINES="${COMPARE_LINES}current ${current#"$ROOT"} sha256=$now
"
    done
    for set in "$BASE"/backups/*; do
        name=${set##*/}
        backup_set_name "$name" || continue
        [ -d "$set" ] && [ ! -L "$set" ] || continue
        complete=1
        for file in $COMPARE_FILES; do
            before=$set/$file.before
            if [ -f "$before" ] && [ ! -L "$before" ] && [ -f "$before.sha256" ] && [ ! -L "$before.sha256" ]; then
                recorded=$(first_field "$before.sha256")
                actual=$(hash "$before") || actual=unreadable
                [ "$recorded" = "$actual" ] || complete=0
                # An earlier owned block or token is not the vehicle's original.
                if grep -F -e 'MX5DR ONE-BOOT' -e '/mx5-aa-dr/' -e 'libmx5dr' "$before" >/dev/null 2>&1; then complete=0; fi
            else
                complete=0
            fi
        done
        if [ "$complete" = 0 ]; then INCOMPLETE="$INCOMPLETE $name"; continue; fi
        COMPLETE=$((COMPLETE + 1))
        SETS="$SETS $name"
        [ -n "$BASIS" ] || BASIS=$name
        match=yes
        for file in $COMPARE_FILES; do
            was=$(first_field "$set/$file.before.sha256")
            [ "$was" = "$(first_field "$BASE/backups/$BASIS/$file.before.sha256")" ] || AGREE=no
            current=$(oem_path "$file")
            if [ "$(now_of "$file")" = "$was" ]; then state=identical; else state=differs; match=no; fi
            COMPARE_LINES="${COMPARE_LINES}compare $name ${current#"$ROOT"} $state pre_install=$was
"
        done
        [ "$match" = no ] || MATCHING="$MATCHING $name"
    done
    if [ "$COMPLETE" = 0 ]; then
        VERDICT='no complete pre-install record; comparison unavailable'
    elif [ "$AGREE" = no ]; then
        VERDICT="recorded pre-install states disagree:$SETS; current files match:${MATCHING:- none} (nothing restored)"
    elif [ -n "$MATCHING" ]; then
        VERDICT="identical to all $COMPLETE recorded pre-install states"
    else
        for file in $COMPARE_FILES; do
            current=$(oem_path "$file")
            [ "$(now_of "$file")" = "$(first_field "$BASE/backups/$BASIS/$file.before.sha256")" ] ||
                DIFFERS="$DIFFERS ${current#"$ROOT"}"
        done
        VERDICT="differs from the pre-install state:$DIFFERS (changed after installation, e.g. by another tool such as the touch mod; left as it is, not restored)"
    fi
}

# --- 4. USB report ----------------------------------------------------------
REPORT=$usb/purge-result.txt
PREVIOUS=''
if [ -f "$REPORT" ] && [ ! -L "$REPORT" ]; then PREVIOUS=$(head -c 32768 "$REPORT") || PREVIOUS=''; fi
save_report() {
    mount_rw "$usb"
    if [ -e "$REPORT" ] || [ -L "$REPORT" ]; then regular "$REPORT"; fi
    report_tmp=$(mktemp "$usb/purge-result.XXXXXX") || fail 'Cannot stage purge-result.txt on the USB'
    if ! {
        printf 'purge_schema=1\nstatus=%s\nboot_id=%s\n' "$1" "$BOOT_NOW"
        printf 'package_directory=%s\n' "${BASE#"$ROOT"}"
        printf 'stale_install_lock_reclaimed=%s\n' "$STALE_LOCK"
        printf 'compare_records=%s\n' "${SETS:- none}"
        printf 'compare_current_matches=%s\n' "${MATCHING:- none}"
        printf 'compare_complete_records=%s (set names use the CMU clock; name order is nominal)\n' "$COMPLETE"
        printf 'compare_records_agree=%s\n' "$AGREE"
        printf 'compare_incomplete_or_not_original=%s\n' "${INCOMPLETE:- none}"
        printf '%s' "$COMPARE_LINES"
        printf 'verdict: %s\n' "$VERDICT"
        printf 'delete_list (kind bytes path):\n%s\n' "$PLANNED"
        printf 'delete_total_files=%s delete_total_bytes=%s\n' "$PLANNED_FILES" "$PLANNED_BYTES"
        printf 'not_deleted_unexpected:\n%s\n' "${UNEXPECTED:-none}"
        printf '%s' "$2"
        if [ -n "$PREVIOUS" ]; then printf '%s\n%s\n' '---- previous purge-result.txt ----' "$PREVIOUS"; fi
    } > "$report_tmp"; then
        rm -f "$report_tmp" || :
        fail 'Cannot write purge-result.txt on the USB'
    fi
    mv -f "$report_tmp" "$REPORT" || { rm -f "$report_tmp" || :; fail 'Cannot replace purge-result.txt'; }
    sync
}

check_uninstalled
check_lock
external_pass count
UNEXPECTED=$(external_pass unexpected)
if [ ! -e "$BASE" ] && [ "$EXTERNAL_COUNT" = 0 ] && [ "$STALE_LOCK" = no ]; then
    echo 'Nothing to delete: the package'
    echo 'directory and its other files are'
    echo 'already absent. No file was changed.'
    [ -z "$UNEXPECTED" ] || printf '%s\n' "$UNEXPECTED"
    exit 0
fi
check_submounts
check_processes
check_collector
BOOT_NOW=$(cat "$ROOT/proc/sys/kernel/random/boot_id" 2>/dev/null) || BOOT_NOW=unavailable
PLANNED=$(inventory)
PLANNED_FILES=$(printf '%s\n' "$PLANNED" | awk '$1=="file" {n++} END {print n+0}')
PLANNED_BYTES=$(printf '%s\n' "$PLANNED" | awk '$1=="file" {s+=$2} END {printf "%.0f\n", s}')
compare_pre_install

# --- 5. Delete --------------------------------------------------------------
# Autostart and both service configs no longer reference the package (checked
# above and again under the lock), so every intermediate state after a power
# cut is inert; running menu 6 again completes the removal.
mount_rw "$persist"
if [ "$STALE_LOCK" = yes ]; then
    rm -f "$LOCK/pid"
    rmdir "$LOCK" || fail 'Cannot reclaim the stale install lock'
    sync
fi
lock
check_uninstalled
check_submounts
check_collector
# Record the comparison before anything is deleted: it needs the backups.
save_report started 'result=interrupted unless a finished report replaces this one
'
external_pass delete
sync
if [ -e "$BASE" ]; then
    # Guard first: nothing may select a partial package even if a later tool
    # restored an owned block. Then everything else.
    rm -rf "$BASE/guard" || fail 'Cannot remove the guard directory'
    sync
    rm -rf "$BASE" || fail 'Cannot remove the package directory'
    sync
fi
absent=absent
if [ -e "$BASE" ] || [ -L "$BASE" ]; then absent=present; fi
remaining=$(external_pass remaining)
rm -f "$LOCK/pid"
rmdir "$LOCK"
LOCKED=0
sync
lock_left=absent
if [ -e "$LOCK" ] || [ -L "$LOCK" ]; then lock_left=present; fi
status=finished
[ "$absent" = absent ] && [ -z "$remaining" ] && [ "$lock_left" = absent ] || status=incomplete
save_report "$status" "after_package_directory=$absent
after_install_lock=$lock_left
after_other_files_remaining=${remaining:- none}
result=$status
"
printf '%s\n' '---- DELETE RESULT ----'
echo "Deleted $PLANNED_FILES files, $PLANNED_BYTES bytes"
if [ "$absent" = absent ]; then echo 'Package directory absent'; else echo 'Package directory STILL PRESENT'; fi
[ -z "$remaining" ] || echo "Still present:$remaining"
if [ "$COMPLETE" = 0 ]; then
    echo 'OEM files: no pre-install record'
elif [ "$AGREE" = no ]; then
    echo 'Pre-install records disagree.'
    echo 'Nothing restored. Current files match:'
    if [ -z "$MATCHING" ]; then echo '  none'; else for name in $MATCHING; do echo "  $name"; done; fi
elif [ -n "$MATCHING" ]; then
    echo 'OEM files: identical to pre-install'
    echo "(all $COMPLETE recorded states agree)"
else
    echo 'OEM files differ from pre-install:'
    for path in $DIFFERS; do echo "  ${path##*/} (kept as it is)"; done
fi
echo 'Saved: purge-result.txt'
[ "$status" = finished ] || fail 'Deletion incomplete; see purge-result.txt'
