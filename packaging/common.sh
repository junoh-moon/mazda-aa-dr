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
LOCK=$ROOT/data_persist/.mx5dr-install-lock
MOUNT_LOCK=$ROOT/tmp/.mx5dr-mount.lock
MOUNT_LOCKED=0
REMOUNTED=''
LOCKED=0
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
    trap - 0 HUP INT TERM
    if [ "$LOCKED" = 1 ]; then
        # Only this process's staging names, never another install's backups.
        for staged in "$ROOT/usr/bin/autostart.mx5dr-new.$$" "$ROOT/usr/bin/autostart.mx5dr-remove.$$" \
            "$ROOT/jci/sm/sm.conf.mx5dr-remove.$$" "$ROOT/jci/sm/sm_WCP.conf.mx5dr-remove.$$" \
            "$BASE"/*.new.$$ "$BASE/guard"/*.new.$$ "$BASE/tools"/*.new.$$ \
            "$ROOT/jci/sm"/*.mx5dr-remove.$$.tap.$$ "$BASE/guard"/*.new.$$.tap.$$; do
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
    mkdir "$LOCK" || fail 'Installer already active or stale lock; inspect recorded PID before recovery'
    LOCKED=1; echo "$$" > "$LOCK/pid"
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
    [ -z "$ROOT" ] || return 0
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
    # 1 MiB for helpers, configuration backups/staging and filesystem overhead.
    # $1 is the new binary payload size; existing mapped binaries may persist.
    validate_persist
    space_free=$(storage_free_kib "$persist") || fail 'Cannot inspect persistent free space'
    space_missing=0
    if [ "$MODE" != OFF ]; then
        [ ! -L "$BASE" ] && [ ! -L "$BASE/logs" ] || fail 'Symlink log storage'
        for entry in trace.0.jsonl:8388608 trace.1.jsonl:8388608 trace.2.jsonl:8388608 \
                     collector.0.jsonl:1048576 collector.1.jsonl:1048576; do
            name=${entry%:*}; cap=${entry#*:}; bytes=0
            if [ -e "$BASE/logs/$name" ] || [ -L "$BASE/logs/$name" ]; then
                regular "$BASE/logs/$name"
                bytes=$(wc -c < "$BASE/logs/$name")
                [ "$bytes" -le "$cap" ] || bytes=$cap
            fi
            space_missing=$((space_missing + cap - bytes))
        done
    fi
    space_need=$((8192 + 64 + 1024 + (space_missing + $1 + 1023) / 1024))
    echo "storage_available_kib=$space_free required_kib=$space_need remaining_log_kib=$(((space_missing + 1023) / 1024)) reserve_kib=8192"
    [ "$space_free" -ge "$space_need" ] || fail 'Insufficient persistent space; installation/rearm not started. Export existing logs and inspect storage while parked.'
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
        collector_files='collector.lock collector.pid collector.0.jsonl collector.1.jsonl'
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
}
trial_to() {
    regular "$1"
    awk -v action=add -v token="$TOKEN" -f "$HERE/edit_service.awk" "$1" > "$2" || fail "Unsupported AA service configuration: $1"
    if [ "$MODE" = SHADOW ]; then
        awk -v action=add -v target_service=jciVBS -v token="$TAP_TOKEN" -f "$HERE/edit_service.awk" "$2" > "$2.tap.$$" || fail "Unsupported VBS service configuration: $1"
        cat "$2.tap.$$" > "$2"
        rm -f "$2.tap.$$"
    fi
    chmod 0600 "$2"
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
    printf 'mode=%s\nmax_log_bytes=8388608\nmax_log_files=3\nsample_ms=1000\n' "$MODE" > "$tmp"
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
        length($0)==36 {
            s=$0
            if (substr(s,9,1)!="-" || substr(s,14,1)!="-" || substr(s,19,1)!="-" || substr(s,24,1)!="-") exit 1
            gsub(/-/,"",s)
            if (length(s)==32 && s ~ /^[0-9a-f]+$/) ok=1
        }
        END {exit !ok}'
}
