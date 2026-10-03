#!/bin/sh
# Run after parking. Archive original files directly to USB; only bounded
# current-boot diagnostics are staged, also on USB. No daemon/guard changes.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/common.sh"
[ "$#" = 1 ] || fail 'Usage: sh export_logs.sh /mounted/destination/directory'
dest=$1
case "$dest" in /*) ;; *) fail 'Destination must be absolute';; esac
[ -d "$dest" ] && [ ! -L "$dest" ] || fail 'Destination is not a real directory'
dest=$(CDPATH= cd -P -- "$dest" && pwd -P) || fail 'Cannot resolve export destination'
persist_ok=0
if persist=$(validate_persist; printf '%s\n' "$persist"); then
    persist_ok=1
    BASE=$persist/mx5-aa-dr
    persist_path=$(CDPATH= cd -P -- "$persist" && pwd -P) || fail 'Cannot resolve persistent storage'
    case "$dest/" in "$persist_path/"*) fail 'Export destination must be outside persistent storage; use the mounted USB';; esac
fi
# A detached USB directory is on CMU tmpfs. Check the actual covering mount
# before creating either staging files or an archive there.
require_export_usb() {
    [ -z "$ROOT" ] || return 0
    awk -v p="$dest" '$2=="/" || p==$2 || index(p,$2"/")==1 {
        if(length($2)>=n){n=length($2);device=$1}}
        END{exit !(device ~ /^\/dev\/sd[a-z]+[0-9]+$/)}' /proc/mounts ||
        fail 'Export destination must be on the mounted USB'
}
require_export_usb
ALLOW_REMOUNT=1
[ -z "$ROOT" ] || ALLOW_REMOUNT=0
mount_rw "$dest"
# Hold the actual USB directory for every output open. If its mount pathname
# disappears later, relative writes stay on that filesystem or fail; they must
# never fall back to the newly exposed CMU directory at the old pathname.
cd -P -- "$dest" || fail 'Cannot enter the USB destination'
require_export_usb
[ "$(stat -c '%d:%i' .)" = "$(stat -c '%d:%i' "$dest")" ] || fail 'USB destination changed'
name=mx5dr-logs-$(date +%Y%m%dT%H%M%S)-$$.tar
[ ! -e "$name" ] || fail 'Export already exists'
stage=$(mktemp -d ./mx5dr-diagnostics-XXXXXX) || fail 'Cannot stage USB diagnostics'
stage_path=$dest/${stage#./}
report=$stage/collection.txt
printf 'export_schema=2\nscope=current_recovery_boot\nretained_scope=all_retained_boots\nsnapshot=sequential_nonatomic\nproc_cap_bytes=131072\nsource_files=read_only\n' > "$report"
printf 'installation_path=%s\ndiagnostics_path=%s\n' "$BASE" "$stage_path" >> "$report"
diagnostic_partial=0

bounded_file() {
    capture_key=$1; capture_source=$2
    mkdir -p "$stage/$(dirname -- "$capture_key")"
    if capture_stat=$(stat -c '%F uid=%u gid=%g mode=%a bytes=%s device=%d inode=%i' "$capture_source" 2>/dev/null); then
        printf '%s.metadata=%s\n' "$capture_key" "$capture_stat" >> "$report"
    fi
    if [ ! -e "$capture_source" ] && [ ! -L "$capture_source" ]; then
        printf '%s=missing\n' "$capture_key" >> "$report"
        return
    fi
    if [ ! -f "$capture_source" ]; then
        printf '%s=metadata_only_not_regular\n' "$capture_key" >> "$report"
        return
    fi
    if [ ! -r "$capture_source" ]; then
        printf '%s=unreadable\n' "$capture_key" >> "$report"
        diagnostic_partial=1
        return
    fi
    # Kernel-owned proc aliases are intentionally readable. Ordinary source
    # links are checked separately; never read FIFO/device nodes here.
    if head -c 131073 "$capture_source" > "$stage/$capture_key.part" 2>/dev/null; then
        capture_bytes=$(wc -c < "$stage/$capture_key.part")
        if [ "$capture_bytes" -gt 131072 ]; then
            head -c 131072 "$stage/$capture_key.part" > "$stage/$capture_key.txt"
            rm -f "$stage/$capture_key.part"
            printf '%s=truncated\n' "$capture_key" >> "$report"
            diagnostic_partial=1
        else
            mv "$stage/$capture_key.part" "$stage/$capture_key.txt"
            printf '%s=captured bytes=%s\n' "$capture_key" "$capture_bytes" >> "$report"
        fi
    else
        mv "$stage/$capture_key.part" "$stage/$capture_key.txt" 2>/dev/null || :
        printf '%s=read_failed\n' "$capture_key" >> "$report"
        diagnostic_partial=1
    fi
}

for source in proc/mounts proc/self/mountinfo proc/sys/kernel/random/boot_id \
              proc/uptime proc/version proc/cmdline proc/meminfo proc/partitions \
              proc/1/status proc/1/cmdline; do
    bounded_file "$source" "$ROOT/$source"
done
if [ ! -L "$HERE/reboot-request.txt" ]; then
    bounded_file usb/reboot-request "$HERE/reboot-request.txt"
fi
if [ ! -L "$HERE/startup-result.txt" ]; then
    bounded_file usb/startup-result "$HERE/startup-result.txt"
fi
if [ "$persist_ok" = 1 ] && [ ! -L "$persist/testmode.conf" ]; then
    bounded_file persist/testmode.conf "$persist/testmode.conf"
fi
for source in tmp/smevents.txt tmp/redirlogs/stdout tmp/redirlogs/stderr; do
    if [ -L "$ROOT/$source" ]; then
        printf '%s=symlink_not_read\n' "$source" >> "$report"
    else bounded_file "$source" "$ROOT/$source"; fi
done

# Inspect only the relevant process metadata, without reading environments or
# invoking a service. Real proc files can have zero stat size and nonzero data.
process_count=0
for proc_dir in "$ROOT"/proc/[0-9]*; do
    [ -d "$proc_dir" ] && [ ! -L "$proc_dir" ] && [ -f "$proc_dir/comm" ] || continue
    process_name=$(head -c 64 "$proc_dir/comm") || continue
    case "$process_name" in
        init_cmu|autostart|sm|mx5dr-collector|aap_service) ;;
        sm_svclauncher) # keep only the three services this package touches
            launcher_args=$(tr '\000' ' ' < "$proc_dir/cmdline" 2>/dev/null | head -c 512) || continue
            case "$launcher_args" in *" jciAAPA "*|*" jciLDS "*|*" jciVBS "*) ;; *) continue;; esac;;
        *) continue;;
    esac
    process_count=$((process_count + 1))
    if [ "$process_count" -gt 64 ]; then
        printf 'process_metadata=truncated count_limit=64\n' >> "$report"
        diagnostic_partial=1
        break
    fi
    for detail in comm cmdline status maps limits; do
        bounded_file "proc/${proc_dir##*/}/$detail" "$proc_dir/$detail"
    done
done

oem_log_directory() {
    log_label=$1; log_directory=$2
    if [ ! -d "$log_directory" ] || [ -L "$log_directory" ]; then
        printf 'oem_logs.%s=unavailable\n' "$log_label" >> "$report"
        return
    fi
    log_count=0
    for log_file in "$log_directory"/*; do
        [ -e "$log_file" ] || [ -L "$log_file" ] || continue
        log_count=$((log_count + 1))
        if [ "$log_count" -gt 16 ]; then
            printf 'oem_logs.%s=truncated file_limit=16\n' "$log_label" >> "$report"
            diagnostic_partial=1
            break
        fi
        log_name=${log_file##*/}
        case "$log_name" in *[!A-Za-z0-9._-]*)
            printf 'oem_logs.%s=unsupported_filename\n' "$log_label" >> "$report"
            diagnostic_partial=1; continue;; esac
        if [ ! -f "$log_file" ] || [ -L "$log_file" ]; then
            printf 'oem_logs.%s.%s=not_regular\n' "$log_label" "$log_name" >> "$report"
            continue
        fi
        mkdir -p "$stage/oem-logs/$log_label"
        if tail -c 131072 "$log_file" > "$stage/oem-logs/$log_label/$log_name.txt" 2>/dev/null; then
            printf 'oem_logs.%s.%s=tail_captured cap_bytes=131072 source_bytes=%s\n' \
                "$log_label" "$log_name" "$(stat -c %s "$log_file" 2>/dev/null || echo unknown)" >> "$report"
        else
            printf 'oem_logs.%s.%s=read_failed\n' "$log_label" "$log_name" >> "$report"
            diagnostic_partial=1
        fi
    done
}
# The stock redirection endpoints above are FIFOs, not readable log files.
# Read bounded tails of the two actual OEM sinks; do not run log-save or make
# a second persistent archive. Their boot/session provenance is not inferred.
printf 'oem_log_scope=retained_unknown_boot\n' >> "$report"
oem_log_directory running "$ROOT/var/log/running_log"
[ "$persist_ok" != 1 ] || oem_log_directory errors "$persist/log/error_logs"

bounded_command() {
    capture_key=$1; shift
    # No OEM program, guard or service is executed. Limit output even if a
    # kernel log has grown. A producer's exit is recorded independently.
    (if "$@"; then capture_rc=0; else capture_rc=$?; fi
     printf '%s\n' "$capture_rc" > "$stage/$capture_key.exit") 2>&1 |
        head -c 131073 > "$stage/$capture_key.command"
    bounded_file "$capture_key" "$stage/$capture_key.command"
    rm -f "$stage/$capture_key.command"
}
if [ -z "$ROOT" ]; then
    bounded_command uname uname -a
    bounded_command processes ps
    bounded_command kernel-log dmesg
    bounded_command usb-space df -Pk "$dest"
    [ "$persist_ok" != 1 ] || bounded_command persist-space df -Pk "$persist"
else
    printf 'commands=fixture_not_executed\n' >> "$report"
fi
if sh "$HERE/startup_diagnostics.sh" > "$stage/startup-diagnostics.txt" 2>&1; then
    diagnostics_rc=0
else diagnostics_rc=$?; diagnostic_partial=1; fi
printf 'startup_diagnostics_exit=%s\n' "$diagnostics_rc" >> "$report"

# Stock tar supports one -C and no append. Root-relative source paths let it
# retain actual UID/GID/mode and link types without copying the installation
# to FAT or reading a symlink target. The USB output is outside every input.
set -- "${stage_path#/}"
if [ "$persist_ok" = 1 ] && [ -d "$BASE" ] && [ ! -L "$BASE" ]; then
    set -- "$@" "${BASE#/}"
    printf 'installation=whole_tree\n' >> "$report"
else
    printf 'installation=missing\n' >> "$report"
    diagnostic_partial=1
fi
for source in usr/bin/autostart jci/sm/sm.conf jci/sm/sm_WCP.conf jci/version.ini; do
    source_path=$ROOT/$source
    case "$source" in
        usr/*) parent_a=$ROOT/usr; parent_b=$ROOT/usr/bin;;
        jci/sm/*) parent_a=$ROOT/jci; parent_b=$ROOT/jci/sm;;
        *) parent_a=$ROOT/jci; parent_b=$ROOT/jci;;
    esac
    if [ -L "$parent_a" ] || [ -L "$parent_b" ]; then
        printf '%s=parent_symlink_not_followed\n' "$source" >> "$report"
        diagnostic_partial=1
    elif [ -e "$source_path" ] || [ -L "$source_path" ]; then
        set -- "$@" "${source_path#/}"
        printf '%s=captured\n' "$source" >> "$report"
    else
        printf '%s=missing\n' "$source" >> "$report"
        diagnostic_partial=1
    fi
done
for source_path in "$ROOT"/tmp/mx5dr-trial-??????; do
    if [ -d "$source_path" ] && [ ! -L "$source_path" ]; then
        set -- "$@" "${source_path#/}"
        printf 'active_trial=%s\n' "$source_path" >> "$report"
    fi
done
printf 'diagnostic_partial=%s\n' "$diagnostic_partial" >> "$report"
if tar -cf - -C / "$@" > "$name.partial"; then tar_rc=0; else tar_rc=$?; fi
if [ "$tar_rc" != 0 ]; then
    echo "archive_tar_exit=$tar_rc; partial archive and USB diagnostics retained: $stage_path"
    if digest=$(hash "$name.partial"); then
        printf '%s  %s.partial\n' "$digest" "$name" > "$name.partial.sha256"
    fi
    fail 'Archive is incomplete; retain .partial, its checksum and the USB diagnostic directory'
fi
mv "$name.partial" "$name"
digest=$(hash "$name") || fail 'Export checksum failed; archive retained'
printf '%s  %s\n' "$digest" "$name" > "$name.sha256"
rm -rf "$stage"
sync
echo "$dest/$name"
echo "diagnostic_partial=$diagnostic_partial (missing/truncated inputs are listed inside collection.txt)"
echo 'Archive includes the whole installation, actual startup/SM files and bounded current-boot diagnostics. Keep it private.'
echo 'Live writer may have rotated during export; partial final JSONL records are possible. Originals retained.'
