#!/bin/sh
# Recovery-time observations only. Never invoke the guard, collector or a hash
# target, consume an arm, acquire a lock, stop a writer, or mount a filesystem.
# common.sh's existing hash fallback alone copies the bundled SHA helper to an
# exclusive /tmp directory, runs that read-only helper, and removes it on exit.
printf 'diagnostic_schema=1\nscope=current_recovery_boot\n'
HERE=$(CDPATH= cd -P -- "$(dirname -- "$0")" && pwd -P) || exit 1
. "$HERE/common.sh"
LC_ALL=C
export LC_ALL
partial=0
printf 'history=not_inferred\nsnapshot=sequential_nonatomic\nguard_check=not_run\nmarker_validation=format_only\n'
if [ "$#" != 0 ]; then
    printf 'arguments=unsupported\ndiagnostic_status=partial\n'
    exit 1
fi
if command -v sha256sum >/dev/null 2>&1; then
    printf 'hash_backend=native_sha256sum\n'
else
    printf 'hash_backend=temporary_bundled_helper\n'
fi

kind_of() {
    if [ -L "$1" ]; then kind=symlink
    elif [ -d "$1" ]; then kind=directory
    elif [ -f "$1" ]; then kind=file
    elif [ -p "$1" ]; then kind=fifo
    elif [ -e "$1" ]; then kind=other
    else kind=missing; fi
}
path_info() {
    pi_key=$1; pi_path=$2
    kind_of "$pi_path"
    printf 'path.%s.type=%s\n' "$pi_key" "$kind"
    if [ "$kind" != missing ]; then
        if pi_stat=$(stat -c '%u %g %a %d %i %s' "$pi_path" 2>/dev/null) &&
            printf '%s\n' "$pi_stat" | awk 'NF==6 {for(i=1;i<=6;i++)if($i !~ /^[0-9]+$/)exit 1;ok=1} END{if(!ok)exit 1}'; then
            set -- $pi_stat
            printf 'path.%s.uid=%s\npath.%s.gid=%s\npath.%s.mode=%s\npath.%s.device=%s\npath.%s.inode=%s\n' \
                "$pi_key" "$1" "$pi_key" "$2" "$pi_key" "$3" "$pi_key" "$4" "$pi_key" "$5"
        else
            printf 'path.%s.stat=unavailable\n' "$pi_key"; partial=1
        fi
    fi
}
# Preserve trailing newlines for strict format/size validation. At most cap+1
# bytes are read, including from procfs files whose stat size is normally zero.
read_small() {
    small=''; read_state=ok
    if [ ! -f "$1" ] || [ ! -r "$1" ]; then read_state=unreadable; return 1; fi
    if small=$(dd if="$1" bs=1 count="$(($2 + 1))" 2>/dev/null || exit 1; printf '.'); then
        small=${small%.}
        if [ "${#small}" -gt "$2" ]; then read_state=oversized; return 1; fi
    else read_state=unreadable; return 1; fi
}
marker() {
    mk_key=$1; mk_file=$2; mk_format=$3; mk_parent=$4
    if [ "$mk_parent" != 1 ]; then printf '%s.status=parent_unavailable\n' "$mk_key"; return; fi
    path_info "$mk_key" "$mk_file"
    kind_of "$mk_file"
    if [ "$kind" = missing ]; then printf '%s.status=missing\n' "$mk_key"; return; fi
    if [ "$kind" != file ]; then printf '%s.status=not_regular\n' "$mk_key"; partial=1; return; fi
    case "$mk_format" in boot) mk_cap=37;; sha) mk_cap=65;; manifest) mk_cap=473;; esac
    if ! mk_size=$(stat -c %s "$mk_file" 2>/dev/null); then mk_size=invalid; fi
    case "$mk_size" in ''|*[!0-9]*) printf '%s.status=unreadable\n' "$mk_key"; partial=1; return;; esac
    if [ "$mk_size" -gt "$mk_cap" ]; then printf '%s.status=oversized\n' "$mk_key"; partial=1; return; fi
    if ! read_small "$mk_file" "$mk_cap"; then
        printf '%s.status=%s\n' "$mk_key" "$read_state"; partial=1; return
    fi
    # Real proc boot_id reports st_size=0. Ordinary marker files must match the
    # bytes read as well, so NUL removal in a shell cannot validate a bad file.
    if [ "${#small}" != "$mk_cap" ] || { [ "$mk_size" != 0 ] && [ "$mk_size" != "${#small}" ]; }; then
        printf '%s.status=malformed\n' "$mk_key"; partial=1; return
    fi
    if mk_value=$(printf '%s' "$small" | awk -v fmt="$mk_format" '
        function sha(s){return length(s)==64 && s ~ /^[0-9a-f]+$/}
        {v[NR]=$0}
        END {
            if(fmt=="manifest") {
                if(NR!=8 || v[1]!="mx5dr-one-boot-v2")exit 1
                for(i=2;i<=8;i++)if(!sha(v[i]))exit 1
                for(i=2;i<=8;i++)print v[i]
            } else if(fmt=="sha") {if(NR!=1 || !sha(v[1]))exit 1; print v[1]}
            else {
                s=v[1]; if(NR!=1 || length(s)!=36 || substr(s,9,1)!="-" || substr(s,14,1)!="-" || substr(s,19,1)!="-" || substr(s,24,1)!="-")exit 1
                gsub(/-/,"",s); if(length(s)!=32 || s !~ /^[0-9a-f]+$/)exit 1
                print v[1]
            }
        }'); then
        printf '%s.status=valid\n' "$mk_key"
        if [ "$mk_format" = manifest ]; then
            mk_i=0
            for mk_sha in $mk_value; do mk_i=$((mk_i + 1)); printf '%s.input_%s=%s\n' "$mk_key" "$mk_i" "$mk_sha"; done
        else printf '%s.value=%s\n' "$mk_key" "$mk_value"; fi
    else printf '%s.status=malformed\n' "$mk_key"; partial=1; fi
}

path_info data_persist "$ROOT/data_persist"
path_info mnt "$ROOT/mnt"
path_info tmp "$ROOT/tmp"
persist=$ROOT/data_persist
persist_ok=1
if [ -L "$persist" ]; then
    case "$(readlink "$persist" 2>/dev/null)" in
        /mnt/data_persist|mnt/data_persist) persist=$ROOT/mnt/data_persist; printf 'alias.data_persist=/mnt/data_persist\n';;
        *) printf 'alias.data_persist=unexpected_redacted\n'; persist_ok=0;;
    esac
    if [ "$persist_ok" = 1 ] && [ -L "$ROOT/mnt" ]; then
        case "$(readlink "$ROOT/mnt" 2>/dev/null)" in
            /tmp/mnt|tmp/mnt)
                persist=$ROOT/tmp/mnt/data_persist; printf 'alias.mnt=/tmp/mnt\n'
                for parent in "$ROOT/tmp" "$ROOT/tmp/mnt"; do
                    [ -d "$parent" ] && [ ! -L "$parent" ] || persist_ok=0
                done;;
            *) printf 'alias.mnt=unexpected_redacted\n'; persist_ok=0;;
        esac
    fi
fi
if [ "$persist_ok" = 1 ] && [ -d "$persist" ] && [ ! -L "$persist" ] &&
    resolved=$(CDPATH= cd -P -- "$persist" && pwd -P); then
    case "$resolved" in "$ROOT"/*) resolved=${resolved#"$ROOT"};; *) persist_ok=0;; esac
    case "$resolved" in /*) ;; *) persist_ok=0;; esac
    case "$resolved" in *[!A-Za-z0-9_./-]*) persist_ok=0;; esac
    [ "${#resolved}" -le 256 ] || persist_ok=0
else persist_ok=0; fi
if [ "$persist_ok" = 1 ]; then
    printf 'persist_resolution=observed\npersist_resolved=%s\n' "$resolved"
    BASE=$persist/mx5-aa-dr
    path_info persist "$persist"
    path_info base "$BASE"
else printf 'persist_resolution=unavailable\n'; partial=1; fi

# Select the longest covering mount; an equal-length later row replaces an
# earlier row. Record this recovery namespace only, never infer flash lifetime
# from a /tmp prefix or treat free bytes as device identity.
mount_found=0
for table in mountinfo mounts; do
    if [ "$table" = mountinfo ]; then table_path=$ROOT/proc/self/mountinfo
    else table_path=$ROOT/proc/mounts; fi
    if [ ! -e "$table_path" ] && [ ! -L "$table_path" ]; then
        printf '%s.status=missing\n' "$table"; continue
    fi
    # Kernel proc aliases are intentionally allowed; ordinary inputs use the
    # strict no-final-symlink rule. A FIFO never passes this regular-file check.
    if ! read_small "$table_path" 65536; then
        printf '%s.status=%s\n' "$table" "$read_state"; partial=1; continue
    fi
    if mount_record=$(printf '%s' "$small" | awk -v format="$table" -v p="${resolved:-}" '
        function safe(s){return length(s)<=256 && s ~ /^[A-Za-z0-9_\/.:+-]+$/}
        {
            if(NR>1024){bad=1;next}
            if(format=="mountinfo") {
                sep=0; for(i=7;i<=NF;i++)if($i=="-"){sep=i;break}
                if(sep==0 || NF<sep+3 || $1 !~ /^[0-9]+$/ || $2 !~ /^[0-9]+$/ || $3 !~ /^[0-9]+:[0-9]+$/){bad=1;next}
                dest=$5; opt=$6","$(sep+3); fs=$(sep+1); dev=$(sep+2)
            } else {
                if(NF!=6){bad=1;next}
                dest=$2; opt=$4; fs=$3; dev=$1
            }
            if(p!="" && (dest=="/" || p==dest || index(p,dest"/")==1) && length(dest)>=longest) {
                if(!safe(dest) || !safe(fs) || !(dev ~ /^\/dev\/[A-Za-z0-9_\/.+-]+$/ || dev ~ /^(tmpfs|rootfs|none|overlay)$/)){bad=1;next}
                access=(","opt"," ~ /,ro,/)?"ro":((","opt"," ~ /,rw,/)?"rw":"unknown")
                noexec=(","opt"," ~ /,noexec,/)?"true":"false"
                longest=length(dest); selected=dev" "dest" "fs" "access" "noexec" "(p==dest?"exact":"ancestor")
            }
        }
        END {if(bad || NR==0)exit 1; if(selected!="")print selected}'); then
        printf '%s.status=read\n' "$table"
        if [ "$mount_found" = 0 ] && [ -n "$mount_record" ]; then
            set -- $mount_record
            printf 'persist_mount.source=%s\npersist_mount.target=%s\npersist_mount.fs=%s\npersist_mount.access=%s\npersist_mount.noexec=%s\npersist_mount.coverage=%s\npersist_mount.table=%s\n' \
                "$1" "$2" "$3" "$4" "$5" "$6" "$table"
            mount_found=1
        fi
    else printf '%s.status=malformed\n' "$table"; partial=1; fi
done
if [ "$mount_found" = 1 ]; then printf 'persist_mount.status=observed\n'
else printf 'persist_mount.status=unavailable\n'; partial=1; fi
printf 'persist_mount.persistence=not_inferred\n'
marker boot_id "$ROOT/proc/sys/kernel/random/boot_id" boot 1

base_ok=0; guard_ok=0; tools_ok=0
if [ "$persist_ok" = 1 ] && [ -d "$BASE" ] && [ ! -L "$BASE" ]; then
    base_ok=1
    path_info logs "$BASE/logs"
    path_info guard "$BASE/guard"
    path_info tools "$BASE/tools"
    if [ -d "$BASE/guard" ] && [ ! -L "$BASE/guard" ]; then guard_ok=1; else partial=1; fi
    if [ -d "$BASE/tools" ] && [ ! -L "$BASE/tools" ]; then tools_ok=1; else partial=1; fi
else partial=1; fi

marker guard.last_boot "$BASE/guard/last-boot" boot "$guard_ok"
marker guard.arm "$BASE/guard/arm" manifest "$guard_ok"
marker guard.consumed "$BASE/guard/consumed" manifest "$guard_ok"
marker guard.normal_source "$BASE/guard/normal.source.sha256" sha "$guard_ok"
marker guard.wcp_source "$BASE/guard/wcp.source.sha256" sha "$guard_ok"
if [ "$guard_ok" = 1 ]; then
    # Match names in find itself, before delimiters can be confused with a name.
    # Only count NUL terminators, at most 65; never retain or echo a filename.
    # The fixed depth also prevents traversal of an unexpected directory.
    if command -v find >/dev/null 2>&1 && command -v head >/dev/null 2>&1 &&
        command -v tr >/dev/null 2>&1 && command -v wc >/dev/null 2>&1 &&
        [ -r "$BASE/guard" ] && [ -x "$BASE/guard" ]; then
        inventory=$({
            {
                if find "$BASE/guard" -mindepth 1 -maxdepth 1 \
                    ! -name last-boot ! -name arm ! -name consumed \
                    ! -name normal.source.sha256 ! -name wcp.source.sha256 \
                    ! -name normal.trial ! -name wcp.trial ! -name mx5dr-guard ! -name lock \
                    -print0 2>/dev/null; then scan_status=0; else scan_status=$?; fi
                # Descriptor 3 bypasses the count pipeline, so an enumeration
                # error cannot become a successful count of zero (POSIX sh).
                printf 'reader=%s\n' "$scan_status" >&3
            } | tr -cd '\000' | head -c 65 | wc -c
        } 3>&1)
        unexpected=$(printf '%s\n' "$inventory" | awk '/^[ \t]*[0-9]+[ \t]*$/{n=$1;c++} END{if(c!=1)print "invalid";else print n+0}')
        scan_status=$(printf '%s\n' "$inventory" | awk -F= '/^reader=[0-9]+$/{n=$2;c++} END{if(c!=1)print "invalid";else print n+0}')
        if [ "$unexpected" = invalid ] || [ "$scan_status" = invalid ]; then
            printf 'guard.inventory.status=unavailable\n'; partial=1
        elif [ "$unexpected" -ge 65 ]; then
            printf 'guard.inventory.status=too_many_entries\nguard.inventory.unexpected_count=at_least_65\n'; partial=1
        elif [ "$scan_status" != 0 ]; then
            printf 'guard.inventory.status=unavailable\n'; partial=1
        else
            printf 'guard.inventory.unexpected_count=%s\n' "$unexpected"
            if [ "$unexpected" = 0 ]; then printf 'guard.inventory.status=known_entries_only\n'
            else printf 'guard.inventory.status=unexpected_entries\n'; partial=1; fi
        fi
    else printf 'guard.inventory.status=unavailable\n'; partial=1; fi
else printf 'guard.inventory.status=parent_unavailable\n'; fi

hash_item() {
    hi_key=$1; hi_file=$2; hi_cap=$3; hi_parent=$4
    if [ "$hi_parent" != 1 ]; then printf 'hash.%s.status=parent_unavailable\n' "$hi_key"; return; fi
    path_info "hash.$hi_key" "$hi_file"
    kind_of "$hi_file"
    if [ "$kind" != file ]; then printf 'hash.%s.status=%s\n' "$hi_key" "$kind"; partial=1; return; fi
    if ! hi_before=$(stat -c '%d:%i:%s:%Y:%Z' "$hi_file" 2>/dev/null) ||
        ! hi_size=$(stat -c %s "$hi_file" 2>/dev/null); then
        printf 'hash.%s.status=stat_failed\n' "$hi_key"; partial=1; return
    fi
    case "$hi_size" in ''|*[!0-9]*) printf 'hash.%s.status=stat_failed\n' "$hi_key"; partial=1; return;; esac
    if [ "$hi_size" -gt "$hi_cap" ]; then printf 'hash.%s.status=oversized\n' "$hi_key"; partial=1; return; fi
    if hi_hash=$(hash "$hi_file" 2>/dev/null); then
        if hi_after=$(stat -c '%d:%i:%s:%Y:%Z' "$hi_file" 2>/dev/null) && [ "$hi_before" = "$hi_after" ]; then
            printf 'hash.%s.status=ok\nhash.%s.sha256=%s\n' "$hi_key" "$hi_key" "$hi_hash"
        else printf 'hash.%s.status=changed_during_read\n' "$hi_key"; partial=1; fi
    else printf 'hash.%s.status=hash_failed\n' "$hi_key"; partial=1; fi
}
# The order below is the guard v2 manifest order. Bodies are never output.
hash_item libmx5dr "$BASE/libmx5dr.so" 33554432 "$base_ok"
hash_item config "$BASE/mx5dr.conf" 1024 "$base_ok"
hash_item sm_normal "$ROOT/jci/sm/sm.conf" 1048576 1
hash_item template_normal "$BASE/guard/normal.trial" 1048576 "$guard_ok"
hash_item sm_wcp "$ROOT/jci/sm/sm_WCP.conf" 1048576 1
hash_item template_wcp "$BASE/guard/wcp.trial" 1048576 "$guard_ok"
hash_item vimtap "$BASE/libmx5dr-vimtap.so" 33554432 "$base_ok"
hash_item autostart "$ROOT/usr/bin/autostart" 1048576 1
hash_item collector "$BASE/mx5dr-collector" 33554432 "$base_ok"
hash_item guard "$BASE/guard/mx5dr-guard" 33554432 "$guard_ok"
for tool in common.sh edit_service.awk edit_autostart.awk arm.sh uninstall.sh export_logs.sh start_collector.sh stop_collector.sh finish_capture.sh trial_status.sh trial_status.awk firmware.sha256 mx5dr-sha256; do
    tool_key=$(printf '%s' "$tool" | tr '.-' '__')
    hash_item "tool_$tool_key" "$BASE/tools/$tool" 1048576 "$tools_ok"
done
if [ "$partial" = 0 ]; then printf 'diagnostic_status=complete\n'; exit 0; fi
printf 'diagnostic_status=partial\n'
exit 1
