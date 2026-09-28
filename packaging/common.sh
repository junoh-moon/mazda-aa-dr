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
LOCK=$ROOT/data_persist/.mx5dr-install-lock
REMOUNTED=''
LOCKED=0
fail() { echo "mx5dr: $*" >&2; exit 1; }
hash() { sha256sum "$1" | awk '{print $1}'; }
regular() { [ -f "$1" ] && [ ! -L "$1" ] || fail "Not a regular non-symlink file: $1"; }
cleanup() {
    rc=$?
    trap - 0 HUP INT TERM
    for mp in $REMOUNTED; do mount -o remount,ro "$mp" || { echo "RESTORE READ-ONLY FAILED: $mp" >&2; rc=1; }; done
    if [ "$LOCKED" = 1 ]; then rm -f "$LOCK/pid"; rmdir "$LOCK" || rc=1; fi
    exit "$rc"
}
trap cleanup 0
trap 'exit 1' HUP INT TERM
lock() {
    [ -d "$ROOT/data_persist" ] && [ ! -L "$ROOT/data_persist" ] || fail 'Missing or symlink data_persist'
    mkdir "$LOCK" || fail 'Installer already active or stale lock; inspect recorded PID before recovery'
    LOCKED=1; echo "$$" > "$LOCK/pid"
}
mount_rw() {
    [ -z "$ROOT" ] || return 0
    mp=$(awk -v p="$1" '$2=="/" || p==$2 || index(p,$2"/")==1 {if(length($2)>n){n=length($2);m=$2;o=$4}} END{print m" "o}' /proc/mounts)
    set -- $mp
    [ "$#" = 2 ] || fail 'Cannot determine mount state'
    case "$1" in *\\*|*[!A-Za-z0-9_./-]*) fail 'Unsupported mount path';; esac
    case ",$2," in
      *,ro,*)
        [ "$ALLOW_REMOUNT" = 1 ] || fail "Read-only mount $1; use --remount from authorized root shell"
        mount -o remount,rw "$1" || fail "Cannot remount $1"
        REMOUNTED="$1 $REMOUNTED"
        ;;
    esac
}
prepare_storage() {
    mount_rw "$ROOT/data_persist"
    lock
    [ ! -L "$BASE" ] || fail 'Symlink installation directory'
    mkdir -p "$BASE" "$BASE/logs" "$BASE/backups"
    [ ! -L "$BASE/logs" ] && [ ! -L "$BASE/backups" ] || fail 'Symlink storage directory'
    chmod 0755 "$BASE" "$BASE/backups"
    chmod 0750 "$BASE/logs"
    if [ -z "$ROOT" ]; then chown cmu "$BASE/logs" || fail 'cmu account unavailable'; fi
}
edit_to() {
    regular "$1"
    cp -p "$1" "$2"
    awk -v action="$3" -v token="$TOKEN" -f "$HERE/edit_service.awk" "$1" > "$2" || fail "Unsupported service configuration: $1"
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
    if [ -z "$ROOT" ]; then chown root "$tmp"; fi
    mv -f "$tmp" "$BASE/mx5dr.conf"
}
