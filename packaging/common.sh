#!/bin/sh
# Sourced by packaging entrypoints. No eval; no execution of firmware files.
set -eu
umask 022
ROOT=${MX5DR_FIXTURE_ROOT:-}
case "$ROOT" in ''|/*) ;; *) echo 'Fixture root must be absolute' >&2; exit 2;; esac
if [ -n "$ROOT" ]; then
    [ "$ROOT" != / ] && [ -f "$ROOT/.mx5dr-fixture" ] || { echo 'Not a marked fixture root' >&2; exit 2; }
    ROOT=${ROOT%/}
fi
[ -n "$ROOT" ] || [ "$(id -u)" = 0 ] || { echo "Root installation shell required" >&2; exit 2; }
BASE=$ROOT/data_persist/mx5-aa-dr
TOKEN=/data_persist/mx5-aa-dr/libmx5dr.so
TAP_TOKEN=/data_persist/mx5-aa-dr/libmx5dr-vimtap.so
LDS_TOKEN=/data_persist/mx5-aa-dr/libmx5dr-ldstap.so
BT_XML=/data_persist/mx5-aa-dr/bt/BdsConfiguration.xml
BT_ARG=" -configFilePath $BT_XML"
LOCK=$ROOT/data_persist/.mx5dr-install-lock
MOUNT_LOCK=$ROOT/tmp/.mx5dr-mount.lock
MOUNT_LOCKED=0
REMOUNTED=''
LOCKED=0
ARM_PENDING=0
PERSIST_PENDING=0
fail() { echo "mx5dr: $*" >&2; exit 1; }
hash() (
    # A stock CMU need not have sha256sum (or its optional -c mode). FAT USB
    # can be noexec and loses executable bits: run our static helper from /tmp.
    if command -v sha256sum >/dev/null 2>&1; then
        result=$(sha256sum "$1") || exit 1
    else
        regular "$HERE/mx5dr-sha256"
        hash_tmp=$(mktemp -d /tmp/mx5dr-hash.XXXXXX) || exit 1
        trap 'rm -f "$hash_tmp/hash"; rmdir "$hash_tmp"' 0
        cp "$HERE/mx5dr-sha256" "$hash_tmp/hash" || exit 1
        chmod 0700 "$hash_tmp/hash" || exit 1
        result=$("$hash_tmp/hash" "$1") || exit 1
    fi
    digest=${result%% *}
    [ "${#digest}" = 64 ] || exit 1
    case "$digest" in *[!0-9a-f]*) exit 1;; esac
    printf '%s\n' "$digest"
)
verify_bundle_manifest() {
    regular "$HERE/SHA256SUMS"
    entries=0
    while read -r expected relative || [ -n "$expected$relative" ]; do
        [ "${#expected}" = 64 ] || fail 'Invalid bundle manifest digest'
        case "$expected" in *[!0-9a-f]*) fail 'Invalid bundle manifest digest';; esac
        case "$relative" in
            ''|/*|..|../*|*/../*|*/..|*[!A-Za-z0-9_./-]*) fail 'Invalid bundle manifest path';;
        esac
        regular "$HERE/$relative"
        actual=$(hash "$HERE/$relative") || fail "Cannot hash bundle file: $relative"
        [ "$actual" = "$expected" ] || fail "Bundle checksum mismatch: $relative; no installation changes made"
        entries=$((entries + 1))
    done < "$HERE/SHA256SUMS"
    [ "$entries" -gt 0 ] || fail 'Empty bundle manifest'
}
regular() { [ -f "$1" ] && [ ! -L "$1" ] || fail "Not a regular non-symlink file: $1"; }
collector_user() {
    # The shipped passwd update names UID 0 "cmu". Use its existing service
    # account for the separate collector; do not rewrite factory accounts.
    cmu_uid=$(id -u cmu) || fail 'cmu account unavailable'
    if [ "$cmu_uid" != 0 ]; then
        printf '%s\n' cmu
    else
        service_uid=$(id -u service) || fail 'service account unavailable'
        [ "$service_uid" != 0 ] || fail 'service must have a nonzero UID'
        printf '%s\n' service
    fi
}
cleanup() {
    rc=$?
    trap - 0
    trap '' HUP INT TERM
    # A caught signal or failed post-arm diagnostic publication must not leave
    # a live authorization. This cannot cover abrupt power loss.
    if [ "$PERSIST_PENDING" = 1 ]; then
        rm -f "$BASE/guard/persist" || rc=1
        sync || rc=1
    fi
    if [ "$ARM_PENDING" = 1 ]; then
        rm -f "$BASE/guard/arm" || rc=1
        rm -f "$BASE/guard/armed-boot.new.$$" || rc=1
        sync || rc=1
    fi
    if [ "$LOCKED" = 1 ]; then
        # Only this process's staging names, never another install's backups.
        for staged in "$ROOT/usr/bin/autostart.mx5dr-new.$$" "$ROOT/usr/bin/autostart.mx5dr-remove.$$" \
            "$ROOT/jci/sm/sm.conf.mx5dr-remove.$$" "$ROOT/jci/sm/sm_WCP.conf.mx5dr-remove.$$" \
            "$BASE"/*.new.$$ "$BASE/guard"/*.new.$$ "$BASE/tools"/*.new.$$ \
            "$ROOT/jci/sm"/*.mx5dr-remove.$$.tap.$$ "$BASE/guard"/*.new.$$.tap.$$ \
            "$ROOT/jci/sm"/*.mx5dr-remove.$$.lds.$$ "$BASE/guard"/*.new.$$.lds.$$ \
            "$BASE/guard"/*.new.$$.bt.$$ "$ROOT/jci/sm"/*.mx5dr-remove.$$.bt.$$ "$BASE/bt"/*.new.$$; do
            [ ! -f "$staged" ] || rm -f "$staged" || rc=1
        done
        rm -f "$LOCK/pid" || rc=1
        rmdir "$LOCK" || rc=1
    fi
    if [ "$LOCKED" = 1 ] || [ -n "$REMOUNTED" ]; then sync || rc=1; fi
    for mp in $REMOUNTED; do mount -o remount,ro "$mp" || { echo "RESTORE READ-ONLY FAILED: $mp" >&2; rc=1; }; done
    # Keep the process lock until restoration finishes. Never unlink its inode:
    # another invocation may already have it open. Exit also releases it on error.
    if [ "$MOUNT_LOCKED" = 1 ]; then exec 9>&-; fi
    exit "$rc"
}
trap cleanup 0
trap 'exit 1' HUP INT TERM
validate_persist() {
    persist=$ROOT/data_persist
    if [ -L "$persist" ]; then
        case "$(readlink "$persist")" in
            /mnt/data_persist|mnt/data_persist) persist=$ROOT/mnt/data_persist;;
            *) fail 'Unexpected data_persist link target';;
        esac
        if [ -L "$ROOT/mnt" ]; then
            case "$(readlink "$ROOT/mnt")" in
                /tmp/mnt|tmp/mnt) persist=$ROOT/tmp/mnt/data_persist;;
                *) fail 'Unexpected mnt link target';;
            esac
        fi
    fi
    [ -d "$persist" ] && [ ! -L "$persist" ] || fail 'Missing persistent storage directory'
    BASE=$persist/mx5-aa-dr
    LOCK=$persist/.mx5dr-install-lock
}
lock() {
    mkdir "$LOCK" || fail 'Install lock present: another menu is running, or an earlier run was interrupted. If nothing else runs, choose 4, then 1 again.'
    LOCKED=1; echo "$$" > "$LOCK/pid"
}
# Menu 4 recovery of an install lock left by an interrupted run (power loss,
# killed shell). Every installer, rearm, uninstall and purge takes the kernel
# mount lock (lock_mounts) before it creates the lock directory and keeps it
# until the directory is removed, so holding that lock ourselves proves no
# live run owns the directory. Without that proof nothing is removed. Only the
# directory and its one pid file are removed; anything else refuses.
reclaim_stale_lock() {
    STALE_LOCK_RECLAIMED=no
    [ -e "$LOCK" ] || [ -L "$LOCK" ] || return 0
    [ -d "$LOCK" ] && [ ! -L "$LOCK" ] || fail 'Install lock is not a directory; nothing changed'
    [ "$MOUNT_LOCKED" = 1 ] || fail 'Install lock present and its owner cannot be checked; another menu may be running. Nothing changed.'
    for stale_entry in "$LOCK"/* "$LOCK"/.[!.]* "$LOCK"/..?*; do
        [ -e "$stale_entry" ] || [ -L "$stale_entry" ] || continue
        [ "$stale_entry" = "$LOCK/pid" ] && [ -f "$stale_entry" ] && [ ! -L "$stale_entry" ] ||
            fail 'Install lock holds unexpected entries; nothing changed'
    done
    rm -f "$LOCK/pid" || fail 'Cannot remove the stale install lock'
    rmdir "$LOCK" || fail 'Cannot remove the stale install lock'
    sync
    STALE_LOCK_RECLAIMED=yes
    echo 'Removed an install lock left by an interrupted run.'
}
lock_mounts() {
    [ "$MOUNT_LOCKED" = 0 ] || return 0
    if [ ! -e "$MOUNT_LOCK" ]; then
        (set -C; umask 077; : > "$MOUNT_LOCK") 2>/dev/null || :
    fi
    regular "$MOUNT_LOCK"
    [ -O "$MOUNT_LOCK" ] || fail 'Mount lock belongs to another UID'
    chmod 0600 "$MOUNT_LOCK"
    exec 9<> "$MOUNT_LOCK"
    flock -n 9 || fail 'Another installer or USB export is active'
    MOUNT_LOCKED=1
}
mount_rw() {
    if [ -n "$ROOT" ]; then
        # Fixtures never remount. MX5DR_FIXTURE_MOUNT_LOCK=1 makes a fixture
        # take the same kernel lock as the CMU (interrupted-install tests).
        [ "${MX5DR_FIXTURE_MOUNT_LOCK:-}" != 1 ] || lock_mounts
        return 0
    fi
    # Stock BusyBox has flock. Serialize before inspecting the mount table,
    # including the gap after persistent lock removal and before remount,ro.
    lock_mounts
    # Follow the CMU's /data_persist alias before selecting its filesystem.
    # /proc/mounts can contain both rootfs / rw and /dev/root / ro. The last
    # equal-length entry is the effective mount, not the first rootfs entry.
    resolved=$(CDPATH= cd -P -- "$1" && pwd -P) || fail "Cannot resolve directory: $1"
    mp=$(awk -v p="$resolved" '$2=="/" || p==$2 || index(p,$2"/")==1 {if(length($2)>=n){n=length($2);m=$2;o=$4}} END{print m" "o}' /proc/mounts)
    set -- $mp
    [ "$#" = 2 ] || fail 'Cannot determine mount state'
    case "$1" in *\\*|*[!A-Za-z0-9_./-]*) fail 'Unsupported mount path';; esac
    case ",$2," in
      *,ro,*)
        [ "$ALLOW_REMOUNT" = 1 ] || fail "Read-only mount $1; remount disabled by --no-remount"
        mount -o remount,rw "$1" || fail "Cannot remount $1"
        REMOUNTED="$1 $REMOUNTED"
        ;;
    esac
}
prepare_storage() {
    validate_persist
    mount_rw "$persist"
    lock
    [ ! -L "$BASE" ] || fail 'Symlink installation directory'
    mkdir -p "$BASE" "$BASE/logs" "$BASE/backups"
    [ ! -L "$BASE/logs" ] && [ ! -L "$BASE/backups" ] || fail 'Symlink storage directory'
    chmod 0755 "$BASE" "$BASE/backups"
    chmod 0750 "$BASE/logs"
}
storage_free_kib() {
    # -P prevents wrapped device rows; -k pins units on both host and OEM BusyBox.
    space_df=$(LC_ALL=C df -Pk "$1") || return 1
    printf '%s\n' "$space_df" | awk 'NR==2 && NF>=6 && $4 ~ /^[0-9]+$/ {free=$4; valid=1}
        END {if(!valid || NR!=2) exit 1; print free}'
}
require_trial_space() {
    # Count only the remaining log allocation, not another full copy of logs
    # already on the volume. Keep 8 MiB + 64 KiB as in runtime/storage.h, plus
    # 1 MiB for helpers, guard files, configuration backups/staging and
    # filesystem overhead. $1 is the new binary payload size; existing mapped
    # binaries may persist. The caps are the ones this installation writes to
    # mx5dr.conf (set_config) and the collector's own caps for that profile:
    # persistent BETA 3 x 16 MiB trace, collector 2 x 1 MiB, phone channel
    # 2 x 1 MiB (counted although it is off by default: menu 11 can turn it on
    # without a new check); one-boot trials 3 x 40 MiB and collector 2 x 4 MiB.
    validate_persist
    space_free=$(storage_free_kib "$persist") || fail 'Cannot inspect persistent free space'
    space_missing=0
    space_reclaim=0
    if [ "${POLICY:-}" = persistent ]; then
        space_trace=16777216; space_collector=1048576
    else
        space_trace=41943040; space_collector=4194304
    fi
    if [ "$MODE" != OFF ]; then
        [ ! -L "$BASE" ] && [ ! -L "$BASE/logs" ] || fail 'Symlink log storage'
        for entry in trace.0.jsonl:$space_trace trace.1.jsonl:$space_trace trace.2.jsonl:$space_trace \
                     collector.0.jsonl:$space_collector collector.1.jsonl:$space_collector \
                     pimu.0.jsonl:1048576 pimu.1.jsonl:1048576; do
            name=${entry%:*}; cap=${entry#*:}; bytes=0
            if [ -e "$BASE/logs/$name" ] || [ -L "$BASE/logs/$name" ]; then
                regular "$BASE/logs/$name"
                bytes=$(wc -c < "$BASE/logs/$name")
                if [ "$bytes" -gt "$cap" ]; then
                    case "${POLICY:-}:$name" in
                        # prune_oversize_traces removes it after the lock.
                        persistent:trace.1.jsonl|persistent:trace.2.jsonl)
                            space_reclaim=$((space_reclaim + bytes)); bytes=0;;
                        *) bytes=$cap;;
                    esac
                fi
            fi
            space_missing=$((space_missing + cap - bytes))
        done
    fi
    space_free=$((space_free + space_reclaim / 1024))
    space_need=$((8192 + 64 + 1024 + (space_missing + $1 + 1023) / 1024))
    echo "storage_available_kib=$space_free required_kib=$space_need remaining_log_kib=$(((space_missing + 1023) / 1024)) reserve_kib=8192 reclaimed_old_trace_kib=$((space_reclaim / 1024))"
    [ "$space_free" -ge "$space_need" ] || fail 'Insufficient persistent space; installation/rearm not started. Export existing logs and inspect storage while parked.'
}
# Persistent installs only: rotated trace files above the persistent cap (the
# 40 MiB files of earlier builds) are removed. trace.0 is never touched: the
# running product may still append to it; the runtime rotates it at its next
# start and the next menu 1 removes it then. Regular files only.
prune_oversize_traces() {
    [ "${POLICY:-}" = persistent ] || return 0
    for name in trace.1.jsonl trace.2.jsonl; do
        old_trace=$BASE/logs/$name
        [ -f "$old_trace" ] && [ ! -L "$old_trace" ] || continue
        old_bytes=$(wc -c < "$old_trace") || continue
        [ "$old_bytes" -gt 16777216 ] || continue
        rm -f "$old_trace" || fail "Cannot remove the oversize old trace file $name"
        echo "Removed old $name ($old_bytes bytes, above the 16 MiB cap of this version)."
    done
}
# Installation backup sets (backups/YYYYMMDDTHHMMSS-<pid>). Prints one line
# "<seq> <original> <name>" per set; seq is the set's tx-seq number, 0 when
# absent or invalid (sets of earlier builds, which are therefore the oldest);
# original is 1 when all three *.before copies exist and carry no package
# block or token (a record of the vehicle before this package, the basis of
# the menu 6 comparison), else 0.
backup_set_list() {
    for backup_set in "$BASE"/backups/*; do
        backup_name=${backup_set##*/}
        case "$backup_name" in
            [0-9][0-9][0-9][0-9][0-9][0-9][0-9][0-9]T[0-9][0-9][0-9][0-9][0-9][0-9]-*) ;;
            *) continue;;
        esac
        case "${backup_name#*-}" in ''|*[!0-9]*) continue;; esac
        [ -d "$backup_set" ] && [ ! -L "$backup_set" ] || continue
        backup_seq=0
        if [ -f "$backup_set/tx-seq" ] && [ ! -L "$backup_set/tx-seq" ]; then
            backup_seq=$(awk 'NR==1 && /^[0-9]+$/ && length($0)<=9 {v=$0} END {print v+0}' "$backup_set/tx-seq" 2>/dev/null) || backup_seq=0
        fi
        backup_original=1
        for backup_file in autostart sm.conf sm_WCP.conf; do
            backup_before=$backup_set/$backup_file.before
            if [ -f "$backup_before" ] && [ ! -L "$backup_before" ]; then
                ! grep -q -e 'MX5DR ONE-BOOT' -e '/mx5-aa-dr/' -e 'libmx5dr' "$backup_before" 2>/dev/null || backup_original=0
            else
                backup_original=0
            fi
            # A record without its digest file is incomplete for menu 6 as well.
            if [ ! -f "$backup_before.sha256" ] || [ -L "$backup_before.sha256" ]; then
                backup_original=0
            fi
        done
        printf '%s %s %s\n' "$backup_seq" "$backup_original" "$backup_name"
    done
}
backup_set_seq_max() {
    backup_set_list | awk 'BEGIN {m=0} $1+0>m {m=$1+0} END {print m}'
}
# Keeps the 3 newest sets (highest seq; equal seq: highest name) and, when
# none of them is an original pre-install record, also the newest original
# one; removes the others. Only directories with a backup set name; never a
# symlink.
prune_backup_sets() {
    backup_old=$(backup_set_list | awk '
        {seq[NR]=$1+0; orig[NR]=$2+0; name[NR]=$3}
        function newer(i, j) { return !j || seq[i]>seq[j] || (seq[i]==seq[j] && name[i]>name[j]) }
        END {
            for (round=1; round<=3; round++) {
                best=0
                for (i=1; i<=NR; i++) if (!kept[i] && newer(i, best)) best=i
                if (best) { kept[best]=1; if (orig[best]) have=1 }
            }
            if (!have) {
                best=0
                for (i=1; i<=NR; i++) if (!kept[i] && orig[i] && newer(i, best)) best=i
                if (best) kept[best]=1
            }
            for (i=1; i<=NR; i++) if (!kept[i]) print name[i]
        }') || return 1
    for backup_name in $backup_old; do
        backup_set=$BASE/backups/$backup_name
        [ -d "$backup_set" ] && [ ! -L "$backup_set" ] || continue
        rm -rf "$backup_set" || fail "Cannot remove the old backup set $backup_name"
    done
}
# Owner switches (markers in the package directory, read once at CMU start by
# the guard, runtime and collector). Experimental features are OFF while their
# marker exists: bt-off (phone Bluetooth channel; the guard then starts the
# stock bds arguments) and g3-garage-off (garage heading carry).
EXPERIMENTAL_MARKERS='bt-off g3-garage-off'
OWNER_MARKERS='vimchan-off g3-off acc-cap-off diag bt-off g3-garage-off'
# The garage heading chain files in logs/ (runtime g3_heading.h): the
# previous-product-boot token and the worker-boot file. Menu 4 removes both;
# menu 1 only the token (a running worker may still use its boot file).
G3_TOKEN_FILES='g3-last-start g3-worker-boot'
# Writes an OFF marker (file content "off"). A symlink of that name is
# removed first (mv would move into a linked directory); a real directory of
# that name already counts as present (OFF) and is left for menu 6.
write_off_marker() {
    off_marker=$BASE/$1
    if [ -d "$off_marker" ] && [ ! -L "$off_marker" ]; then return 0; fi
    if [ -L "$off_marker" ]; then rm -f "$off_marker" || return 1; fi
    off_tmp=$BASE/$1.new.$$
    rm -f "$off_tmp" || return 1
    if ! printf 'off\n' > "$off_tmp" || ! mv -f "$off_tmp" "$off_marker"; then
        rm -f "$off_tmp" || :
        return 1
    fi
}
# Removes one marker or token file (never a directory). Returns 1 on failure.
remove_entry() {
    if [ -f "$1" ] || [ -L "$1" ]; then rm -f "$1" || return 1; fi
    return 0
}
prepare_collector_storage() {
    # Installation/arming need a runnable collector. Recovery must still work
    # after accounts change or disappear, so generic storage has no NSS gate.
    if [ -z "$ROOT" ]; then
        log_user=$(collector_user) || fail 'Cannot select collector account'
    fi
    prepare_storage
    if [ -z "$ROOT" ]; then
        log_uid=$(id -u "$log_user") || fail 'Cannot resolve collector UID'
        collector_lock=$BASE/logs/collector.lock
        if [ ! -e "$collector_lock" ] && [ ! -L "$collector_lock" ]; then
            # Another collector may create the same stable lock first.
            (set -C; umask 077; : > "$collector_lock") 2>/dev/null || :
        fi
        regular "$collector_lock"
        exec 8<> "$collector_lock" || fail 'Cannot open collector ownership lock'
        if ! flock -n 8; then
            # The production collector never unlinks its lock. Do not inspect
            # its mutable PID/journals while it can remove or rotate them.
            log_owner=$(stat -c %u "$BASE/logs") || fail 'Cannot inspect logs ownership'
            lock_owner=$(stat -c %u "$collector_lock") || fail 'Cannot inspect collector lock ownership'
            [ "$log_owner" = "$log_uid" ] && [ "$lock_owner" = "$log_uid" ] ||
                fail 'Collector is active; cannot transfer log ownership'
            exec 8>&-
            return 0
        fi
        # Validate every existing entry while holding the collector's lock,
        # before changing any owner. Never recurse into AA traces or evidence.
        collector_files='collector.lock collector.pid collector.0.jsonl collector.1.jsonl pimu.0.jsonl pimu.1.jsonl bt-fail'
        for collector_name in $collector_files; do
            collector_path=$BASE/logs/$collector_name
            if [ -e "$collector_path" ] || [ -L "$collector_path" ]; then
                regular "$collector_path"
            fi
        done
        for collector_name in $collector_files; do
            collector_path=$BASE/logs/$collector_name
            if [ -f "$collector_path" ]; then
                file_owner=$(stat -c %u "$collector_path") || fail "Cannot inspect ownership: $collector_name"
                if [ "$file_owner" != "$log_uid" ]; then
                    chown "$log_user" "$collector_path" || fail "Cannot transfer ownership: $collector_name"
                fi
            fi
        done
        log_owner=$(stat -c %u "$BASE/logs") || fail 'Cannot inspect logs ownership'
        if [ "$log_owner" != "$log_uid" ]; then
            chown "$log_user" "$BASE/logs" || fail 'Cannot transfer logs directory ownership'
        fi
        exec 8>&-
    fi
}
edit_to() {
    regular "$1"
    cp -p "$1" "$2"
    awk -v action="$3" -v token="$TOKEN" -f "$HERE/edit_service.awk" "$1" > "$2" || fail "Unsupported service configuration: $1"
    if [ "$3" = remove ] && grep -F "$TAP_TOKEN" "$2" >/dev/null; then
        awk -v action=remove -v target_service=jciVBS -v token="$TAP_TOKEN" -f "$HERE/edit_service.awk" "$2" > "$2.tap.$$" || fail "Unsupported VBS service configuration: $1"
        cat "$2.tap.$$" > "$2"
        rm -f "$2.tap.$$"
    fi
    if [ "$3" = remove ] && grep -F "$BT_ARG" "$2" >/dev/null; then
        awk -v what=args -v action=remove -v xml="$BT_XML" -f "$HERE/edit_bds.awk" "$2" > "$2.bt.$$" || fail "Unsupported bds service configuration: $1"
        cat "$2.bt.$$" > "$2"
        rm -f "$2.bt.$$"
    fi
    if [ "$3" = remove ] && grep -F "$LDS_TOKEN" "$2" >/dev/null; then
        awk -v action=remove -v target_service=jciLDS -v token="$LDS_TOKEN" -f "$HERE/edit_service.awk" "$2" > "$2.lds.$$" || fail "Unsupported LDS service configuration: $1"
        cat "$2.lds.$$" > "$2"
        rm -f "$2.lds.$$"
    fi
}
trial_to() {
    regular "$1"
    awk -v action=add -v token="$TOKEN" -f "$HERE/edit_service.awk" "$1" > "$2" || fail "Unsupported AA service configuration: $1"
    if [ "$MODE" != OFF ]; then
        awk -v action=add -v target_service=jciLDS -v token="$LDS_TOKEN" -f "$HERE/edit_service.awk" "$2" > "$2.lds.$$" || fail "Unsupported LDS service configuration: $1"
        cat "$2.lds.$$" > "$2"
        rm -f "$2.lds.$$"
    fi
    # BETA keeps SHADOW capture, so it needs the same VBS sensor tap.
    if [ "$MODE" = SHADOW ] || [ "$MODE" = BETA ]; then
        awk -v action=add -v target_service=jciVBS -v token="$TAP_TOKEN" -f "$HERE/edit_service.awk" "$2" > "$2.tap.$$" || fail "Unsupported VBS service configuration: $1"
        cat "$2.tap.$$" > "$2"
        rm -f "$2.tap.$$"
    fi
    # Phone IMU Bluetooth service (validation/PHONE_IMU_CHANNEL_2026-10-14.md):
    # only the bds args of this trial template; the guard removes it again on
    # every selection unless bt-off/bt-fail are absent and the XML is intact.
    if [ "${BT:-0}" = 1 ]; then
        awk -v what=args -v action=add -v xml="$BT_XML" -f "$HERE/edit_bds.awk" "$2" > "$2.bt.$$" || fail "Unsupported bds service configuration: $1"
        cat "$2.bt.$$" > "$2"
        rm -f "$2.bt.$$"
    fi
    chmod 0600 "$2"
}
# Stock Bluetooth inputs of the phone IMU service: the configuration the XML
# is generated from and the bds binary the offline parse check ran
# (tests/packaging/test_bds_parse_qemu.py). A mismatch only skips the service.
bt_stock_ok() {
    while read -r want name; do
        [ -n "$want" ] || continue
        [ -f "$ROOT/$name" ] && [ ! -L "$ROOT/$name" ] || return 1
        [ "$(hash "$ROOT/$name")" = "$want" ] || return 1
    done < "$HERE/bt.sha256"
}
# Generates BASE/bt/BdsConfiguration.xml (+ .sha256 for the guard) from the
# stock XML with exactly one added serialPort line; never touches /jci/bds.
stage_bt_config() {
    [ ! -L "$BASE/bt" ] || fail 'Symlink bt directory'
    mkdir -p "$BASE/bt"
    chmod 0755 "$BASE/bt"
    if [ -z "$ROOT" ]; then chown 0 "$BASE/bt"; fi
    bt_new=$BASE/bt/BdsConfiguration.xml.new.$$
    awk -v what=xml -f "$HERE/edit_bds.awk" "$ROOT/jci/bds/BdsConfiguration.xml" > "$bt_new" || fail 'Unsupported stock Bluetooth configuration'
    bt_lines_in=$(wc -l < "$ROOT/jci/bds/BdsConfiguration.xml")
    bt_lines_out=$(wc -l < "$bt_new")
    [ "$bt_lines_out" -eq $((bt_lines_in + 1)) ] || fail 'Bluetooth configuration edit is not one added line'
    [ "$(grep -c -F 'name="MX5DR IMU"' "$bt_new")" = 1 ] || fail 'Bluetooth configuration edit is not one added line'
    grep -v -F 'name="MX5DR IMU"' "$bt_new" | cmp -s - "$ROOT/jci/bds/BdsConfiguration.xml" || fail 'Bluetooth configuration edit changed stock lines'
    hash "$bt_new" > "$BASE/bt/BdsConfiguration.xml.sha256.new.$$" || fail 'Cannot hash the Bluetooth configuration'
    chmod 0644 "$bt_new" "$BASE/bt/BdsConfiguration.xml.sha256.new.$$"
    if [ -z "$ROOT" ]; then chown 0 "$bt_new" "$BASE/bt/BdsConfiguration.xml.sha256.new.$$"; fi
}
publish_bt_config() {
    mv -f "$BASE/bt/BdsConfiguration.xml.new.$$" "$BASE/bt/BdsConfiguration.xml"
    mv -f "$BASE/bt/BdsConfiguration.xml.sha256.new.$$" "$BASE/bt/BdsConfiguration.xml.sha256"
}
remove_bt_config() {
    [ ! -L "$BASE/bt" ] || fail 'Symlink bt directory'
    [ -d "$BASE/bt" ] || return 0
    rm -f "$BASE/bt/BdsConfiguration.xml" "$BASE/bt/BdsConfiguration.xml.sha256" || return 1
    rmdir "$BASE/bt" 2>/dev/null || :
}
verify_firmware() {
    regular "$ROOT/jci/version.ini"
    grep -q '^JCI_SW_VER="MAZ_CMU-150_74.00.324"$' "$ROOT/jci/version.ini" || fail 'Wrong firmware version'
    grep -q '^JCI_SW_FLAVOR="cmu150_NA"$' "$ROOT/jci/version.ini" || fail 'Wrong firmware region'
    grep -q '^JCI_SW_VER_PATCH="A"$' "$ROOT/jci/version.ini" || fail 'Wrong firmware patch'
    while read -r want name; do
        [ -n "$want" ] || continue
        regular "$ROOT/$name"
        [ "$(hash "$ROOT/$name")" = "$want" ] || fail "Firmware hash mismatch: $name"
    done < "$HERE/firmware.sha256"
}
set_config() {
    tmp=$BASE/mx5dr.conf.new.$$
    # The always-on BETA install (persistent policy) uses the quiet journal
    # profile and a 48 MiB trace ring (3 x 16 MiB; boots append to trace.0
    # until it is full, so the ring is the newest rows whatever the number
    # of boots, validation/LOG_RETENTION_2026-10-10.md);
    # one-boot trials keep the full profile.
    if [ "${POLICY:-}" = persistent ]; then
        printf 'mode=%s\nmax_log_bytes=16777216\nmax_log_files=3\nsample_ms=1000\nlog_profile=persistent\n' "$MODE" > "$tmp"
    else
        printf 'mode=%s\nmax_log_bytes=41943040\nmax_log_files=3\nsample_ms=1000\n' "$MODE" > "$tmp"
    fi
    chmod 0644 "$tmp"
    if [ -z "$ROOT" ]; then chown 0 "$tmp"; fi
    mv -f "$tmp" "$BASE/mx5dr.conf"
}

# Only explicit installation/rearm may prepare another capture session.
# Never discard journal files or recursively remove an unexpected marker.
clear_capture_markers() {
    if [ -e "$BASE/logs/capture.stop" ] || [ -L "$BASE/logs/capture.stop" ]; then
        [ -d "$BASE/logs/capture.stop" ] && [ ! -L "$BASE/logs/capture.stop" ] || fail 'Invalid capture stop marker'
        rmdir "$BASE/logs/capture.stop" || fail 'Capture stop marker is not empty'
    fi
    if [ -e "$BASE/logs/capture.done" ] || [ -L "$BASE/logs/capture.done" ]; then
        regular "$BASE/logs/capture.done"
        rm "$BASE/logs/capture.done" || fail 'Cannot clear capture acknowledgement'
    fi
}

valid_boot_id() {
    printf '%s\n' "$1" | awk '
        NR==1 {
            s=$0
            if (length(s)!=36 || substr(s,9,1)!="-" || substr(s,14,1)!="-" ||
                substr(s,19,1)!="-" || substr(s,24,1)!="-") bad=1
            gsub(/-/,"",s)
            if (length(s)!=32 || s !~ /^[0-9a-f]+$/) bad=1
        }
        NR>1 {bad=1}
        END {exit (NR!=1 || bad)}'
}
read_boot_record() {
    boot_record_path=$1
    [ -f "$boot_record_path" ] && [ ! -L "$boot_record_path" ] || return 1
    boot_record_bytes=$(wc -c < "$boot_record_path") || return 1
    [ "$boot_record_bytes" -eq 37 ] || return 1
    boot_record_id=$(cat "$boot_record_path") || return 1
    valid_boot_id "$boot_record_id" || return 1
    # Command substitution can discard NUL and trailing LF. Compare the file's
    # actual bytes so neither corruption can become a qualified boot marker.
    boot_record_actual_raw=$(od -v -An -t x1 "$boot_record_path") || return 1
    boot_record_expected_raw=$(printf '%s\n' "$boot_record_id" | od -v -An -t x1) || return 1
    boot_record_actual=$(printf '%s\n' "$boot_record_actual_raw" | tr -d '[:space:]') || return 1
    boot_record_expected=$(printf '%s\n' "$boot_record_expected_raw" | tr -d '[:space:]') || return 1
    [ "${#boot_record_actual}" -eq 74 ] && [ "${#boot_record_expected}" -eq 74 ] || return 1
    [ "$boot_record_actual" = "$boot_record_expected" ] || return 1
    [ "${2:-}" = quiet ] && return 0
    printf '%s\n' "$boot_record_id"
}

# Check before changing service startup or rearm templates. Do not replace a
# previous trial's diagnostic marker until the target guard has actually armed.
prepare_arm_boot() {
    arm_boot_id=$(read_boot_record "$ROOT/proc/sys/kernel/random/boot_id") || fail 'Invalid arming boot ID'
}

# Keep the prior trial's boot ID for export, but never pair it with a new arm.
# A failed retry then remains diagnostically unconfirmed without deleting raw
# logs or the earlier ID.
stash_arm_boot() {
    arm_marker_old=$BASE/guard/armed-boot
    arm_marker_previous=$BASE/guard/armed-boot.previous
    # A malformed old marker is itself a failed rearm. Revoke the old arm on
    # any failure after entering this transaction, including type validation.
    ARM_PENDING=1
    if [ -e "$arm_marker_old" ] || [ -L "$arm_marker_old" ]; then
        read_boot_record "$arm_marker_old" quiet || fail 'Invalid prior arming boot marker; menu 6 removes it, then menu 1'
    fi
    if [ -e "$arm_marker_previous" ] || [ -L "$arm_marker_previous" ]; then
        read_boot_record "$arm_marker_previous" quiet || fail 'Invalid previous arming boot marker; menu 6 removes it, then menu 1'
    fi
    if [ -e "$arm_marker_old" ]; then
        mv -f "$arm_marker_old" "$arm_marker_previous" || fail 'Cannot preserve prior arming boot marker'
        sync
    fi
}

# Diagnostic only: selection still belongs to the target guard at autostart.
# The caller revokes arm if this post-arm publication fails.
record_arm_boot() {
    arm_boot_tmp=$BASE/guard/armed-boot.new.$$
    [ ! -e "$arm_boot_tmp" ] && [ ! -L "$arm_boot_tmp" ] || return 1
    if [ -e "$BASE/guard/armed-boot" ] || [ -L "$BASE/guard/armed-boot" ]; then
        [ -f "$BASE/guard/armed-boot" ] && [ ! -L "$BASE/guard/armed-boot" ] || return 1
    fi
    printf '%s\n' "$arm_boot_id" > "$arm_boot_tmp" || return 1
    chmod 0600 "$arm_boot_tmp" || return 1
    if [ -z "$ROOT" ]; then chown 0 "$arm_boot_tmp" || return 1; fi
    sync || return 1
    mv -f "$arm_boot_tmp" "$BASE/guard/armed-boot" || return 1
    sync || return 1
}
