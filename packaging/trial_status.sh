#!/bin/sh
# Read-only, parked collection check. Never arms, stops, restarts or changes mode.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
. "$HERE/common.sh"
[ "$#" = 0 ] || fail 'Usage: sh trial_status.sh (while parked)'
validate_persist
for directory in "$BASE" "$BASE/logs" "$BASE/guard"; do
    [ -d "$directory" ] && [ ! -L "$directory" ] || fail "Missing or symlink directory: $directory"
done
regular "$HERE/trial_status.awk"
boot_id=$(read_boot_record "$ROOT/proc/sys/kernel/random/boot_id") || fail 'Invalid current boot ID'
# The kernel identity must remain visible even when an optional marker or a
# later journal check is unusable.
echo 'Parked capture evidence only; this does not approve driving or ASSIST.'
echo "current_boot_id=$boot_id. Recent checks can be unavailable after AA disconnect or reboot; retained rows are reported separately."
regular "$ROOT/proc/uptime"
now=$(awk 'NR==1 && $1 ~ /^[0-9]+\.[0-9]+$/ {print $1}' "$ROOT/proc/uptime")
[ -n "$now" ] || fail 'Cannot read current monotonic uptime'
guard_manifest_schema() (
    [ -f "$1" ] && [ ! -L "$1" ] || exit 1
    marker_bytes=$(wc -c < "$1") || exit 1
    marker_bytes=$(printf '%s\n' "$marker_bytes" | tr -d '[:space:]') || exit 1
    # Recover v2 evidence, but only this package's eight-input v3 can
    # qualify a new guarded startup.
    case "$marker_bytes" in
        473) schema=v2; rows=8;;
        538) schema=v3; rows=9;;
        *) exit 1;;
    esac
    # Verify all raw bytes before awk: some implementations discard NUL, and
    # a failed od must not be hidden by output whitespace normalization.
    marker_hex_raw=$(od -v -An -t x1 "$1") || exit 1
    marker_hex=$(printf '%s\n' "$marker_hex_raw" | tr -d '[:space:]') || exit 1
    [ "${#marker_hex}" -eq "$((marker_bytes * 2))" ] || exit 1
    case "$marker_hex" in *0a) ;; *) exit 1;; esac
    printf '%s\n' "$marker_hex" | awk '{for(i=1;i<=length($0);i+=2) if(substr($0,i,2)=="00") exit 1}' || exit 1
    LC_ALL=C awk -v schema="$schema" -v rows="$rows" '
        NR==1 {if($0!="mx5dr-one-boot-" schema) bad=1; next}
        NR>=2 && NR<=rows {
            if(length($0)!=64 || $0 ~ /[^0-9a-f]/) bad=1
            next
        }
        {bad=1}
        END {exit (bad || NR!=rows)}' "$1" || exit 1
    printf '%s\n' "$schema"
)
read_config_mode() (
    config=$BASE/mx5dr.conf
    [ -f "$config" ] && [ ! -L "$config" ] || exit 1
    config_bytes=$(wc -c < "$config") || exit 1
    [ "$config_bytes" -le 1024 ] || exit 1
    config_hex_raw=$(od -v -An -t x1 "$config") || exit 1
    config_hex=$(printf '%s\n' "$config_hex_raw" | tr -d '[:space:]') || exit 1
    [ "${#config_hex}" -eq "$((config_bytes * 2))" ] || exit 1
    printf '%s\n' "$config_hex" | awk '{for(i=1;i<=length($0);i+=2) if(substr($0,i,2)=="00") exit 1}' || exit 1
    LC_ALL=C awk '
        function trim(s) {sub(/^[ \t\r\n]+/,"",s); sub(/[ \t\r\n]+$/,"",s); return s}
        {
            if(length($0)>=255) {bad=1; exit}
            line=$0; sub(/#.*/,"",line); line=trim(line)
            if(line=="") next
            equal=index(line,"=")
            if(!equal) {bad=1; exit}
            key=trim(substr(line,1,equal-1)); value=trim(substr(line,equal+1))
            if(seen[key]++) {bad=1; exit}
            if(key=="mode") {
                if(value!="OFF" && value!="OBSERVE" && value!="SCRUB" && value!="SHADOW" && value!="BETA") {bad=1; exit}
                mode=value; next
            }
            if(value !~ /^[0-9]+$/ || length(value)>12) {bad=1; exit}
            n=value+0
            if(key=="max_log_bytes") {if(n<65536 || n>41943040) bad=1}
            else if(key=="max_log_files") {if(n<1 || n>3) bad=1}
            else if(key=="sample_ms") {if(n<500 || n>5000) bad=1}
            else bad=1
            if(bad) exit
        }
        END {if(bad || mode=="") exit 1; print mode}' "$config"
)
# A guard marker alone proves neither a live process nor successful capture.
guard_last_boot=missing
last_boot=
if [ -e "$BASE/guard/last-boot" ] || [ -L "$BASE/guard/last-boot" ]; then
    if last_boot=$(read_boot_record "$BASE/guard/last-boot"); then
        guard_last_boot=different
        if [ "$last_boot" = "$boot_id" ]; then guard_last_boot=current; fi
    else
        guard_last_boot=invalid
    fi
fi
guard_consumed=absent
guard_consumed_schema=none
if [ -e "$BASE/guard/consumed" ] || [ -L "$BASE/guard/consumed" ]; then
    if guard_consumed_schema=$(guard_manifest_schema "$BASE/guard/consumed"); then
        guard_consumed=present
    else
        guard_consumed=invalid
        guard_consumed_schema=invalid
    fi
fi
guard_armed_boot=missing
armed_boot=
if [ -e "$BASE/guard/armed-boot" ] || [ -L "$BASE/guard/armed-boot" ]; then
    if armed_boot=$(read_boot_record "$BASE/guard/armed-boot"); then
        guard_armed_boot=different
        if [ "$armed_boot" = "$boot_id" ]; then guard_armed_boot=current; fi
    else
        guard_armed_boot=invalid
    fi
fi
guard_previous_armed_boot=missing
if [ -e "$BASE/guard/armed-boot.previous" ] || [ -L "$BASE/guard/armed-boot.previous" ]; then
    if read_boot_record "$BASE/guard/armed-boot.previous" quiet; then
        guard_previous_armed_boot=valid
    else
        guard_previous_armed_boot=invalid
    fi
fi
guard_arm=absent
guard_arm_schema=none
if [ -e "$BASE/guard/arm" ] || [ -L "$BASE/guard/arm" ]; then
    if guard_arm_schema=$(guard_manifest_schema "$BASE/guard/arm"); then
        guard_arm=present
    else
        guard_arm=invalid
        guard_arm_schema=invalid
    fi
fi
oneboot=unconfirmed
startup_state=guard_selection_unconfirmed
if [ "$guard_arm" = present ]; then
    oneboot=arm_present
    if [ "$guard_arm_schema" != v3 ]; then
        startup_state=legacy_guard_manifest
    else
        case "$guard_armed_boot" in
            current) startup_state=awaiting_linux_reboot;;
            different) startup_state=new_linux_boot_arm_unconsumed;;
            *) startup_state=armed_boot_unconfirmed;;
        esac
    fi
elif [ "$guard_arm" = invalid ]; then
    oneboot=arm_invalid
    startup_state=invalid_arm_marker
elif [ "$guard_last_boot" = current ] && [ "$guard_consumed" = present ]; then
    oneboot=consumed_this_boot
    if [ "$guard_consumed_schema" != v3 ]; then
        oneboot=legacy_consumed_this_boot
        startup_state=legacy_guard_manifest
    else
        case "$guard_armed_boot" in
            current) startup_state=guard_selected_same_boot_as_arm;;
            different) startup_state=guard_committed_after_new_boot;;
            *) startup_state=guard_selected_reboot_unconfirmed;;
        esac
    fi
fi
# Persistent BETA policy (v1.0): the guard decides again on every boot from
# root-owned guard/persist and guard/persist-state. Data only; never executed.
no_nul_small() (
    [ -f "$1" ] && [ ! -L "$1" ] || exit 1
    small_bytes=$(wc -c < "$1") || exit 1
    small_bytes=$(printf '%s\n' "$small_bytes" | tr -d '[:space:]') || exit 1
    [ "$small_bytes" -gt 0 ] && [ "$small_bytes" -le 1024 ] || exit 1
    small_hex_raw=$(od -v -An -t x1 "$1") || exit 1
    small_hex=$(printf '%s\n' "$small_hex_raw" | tr -d '[:space:]') || exit 1
    [ "${#small_hex}" -eq "$((small_bytes * 2))" ] || exit 1
    case "$small_hex" in *0a) ;; *) exit 1;; esac
    printf '%s\n' "$small_hex" | awk '{for(i=1;i<=length($0);i+=2) if(substr($0,i,2)=="00") exit 1}'
)
persist_manifest_rule() (
    no_nul_small "$1" || exit 1
    LC_ALL=C awk '
        NR==1 {if($0!="mx5dr-persist-v1") bad=1; next}
        NR==2 {if($0!="mode=BETA") bad=1; next}
        NR==3 {if($0=="healthy_rule=off") rule="off"; else if($0=="healthy_rule=on") rule="on"; else bad=1; next}
        NR==4 {if($0!="mx5dr-one-boot-v3") bad=1; next}
        NR>=5 && NR<=12 {if(length($0)!=64 || $0 ~ /[^0-9a-f]/) bad=1; next}
        {bad=1}
        END {if(bad || NR!=12) exit 1; print rule}' "$1"
)
read_persist_state() (
    no_nul_small "$1" || exit 1
    LC_ALL=C awk '
        function uuid(s) {
            return length(s)==36 && substr(s,9,1)=="-" && substr(s,14,1)=="-" &&
                substr(s,19,1)=="-" && substr(s,24,1)=="-" && s !~ /[^0-9a-f-]/
        }
        function count(s) {return s ~ /^[0-9]+$/ && length(s)<=6 && (s=="0" || s !~ /^0/)}
        BEGIN {n=split("enabled_boot fail_count attempts_since_healthy attempt_boot attempt_reports previous healthy_previous tripped",key," ")}
        NR==1 {if($0!="mx5dr-persist-state-v1") bad=1; next}
        NR>=2 && NR<=n+1 {
            p=index($0,"=")
            if(!p || substr($0,1,p-1)!=key[NR-1]) bad=1
            v[NR-1]=substr($0,p+1); next
        }
        {bad=1}
        END {
            if(bad || NR!=n+1) exit 1
            if(!uuid(v[1]) || !count(v[2]) || !count(v[3]) || v[2]+0>2) exit 1
            if(v[4]!="none" && !uuid(v[4])) exit 1
            if(v[6] !~ /^(none|ok|healthy|failed_reset|failed_bootloop)$/) exit 1
            if(v[7] !~ /^(none|yes|no)$/) exit 1
            if(v[8] !~ /^(no|reset_reports|boot_loop|runtime_disabled)$/) exit 1
            print v[1], v[2], v[3], v[4], v[6], v[7], v[8]
        }' "$1"
)
guard_policy=one-boot
persist_status=absent
persist_reason=none
persist_rule=none
persist_fail=none
persist_attempts=none
persist_previous=none
persist_healthy_previous=none
persist_selected=no
persist_enabled_this_boot=no
if [ -e "$BASE/guard/persist" ] || [ -L "$BASE/guard/persist" ]; then
    guard_policy=persistent
    persist_status=invalid
    if persist_rule=$(persist_manifest_rule "$BASE/guard/persist") &&
       persist_fields=$(read_persist_state "$BASE/guard/persist-state"); then
        set -- $persist_fields
        persist_status=enabled
        persist_fail=$2; persist_attempts=$3; persist_previous=$5; persist_healthy_previous=$6
        if [ "$7" != no ]; then persist_status=tripped; persist_reason=$7; fi
        [ "$1" != "$boot_id" ] || persist_enabled_this_boot=yes
        if [ "$4" = "$boot_id" ] && [ "$guard_last_boot" = current ]; then persist_selected=yes; fi
        set --
    else
        persist_rule=invalid
    fi
    oneboot=unconfirmed
    case "$persist_status:$persist_selected:$persist_enabled_this_boot" in
        tripped:*) startup_state=persistent_tripped;;
        invalid:*) startup_state=persistent_state_invalid;;
        enabled:yes:*) oneboot=persistent_this_boot; startup_state=guard_committed_persistent;;
        enabled:no:yes) startup_state=persistent_enabled_this_boot;;
        *) startup_state=persistent_not_selected;;
    esac
fi
echo "guard_policy=$guard_policy persist=$persist_status persist_reason=$persist_reason persist_healthy_rule=$persist_rule"
echo "persist_fail_count=$persist_fail persist_trip_at=2 persist_attempts_since_healthy=$persist_attempts persist_previous=$persist_previous persist_healthy_previous=$persist_healthy_previous persist_selected_this_boot=$persist_selected persist_enabled_this_boot=$persist_enabled_this_boot"
# Removal keeps prior guard evidence for export. A disabled, missing, damaged
# or noncanonical config cannot borrow a prior boot's positive status.
config_mode=unconfirmed
if config_mode=$(read_config_mode); then :; else config_mode=unconfirmed; fi
case "$config_mode" in
    OFF) oneboot=unconfirmed; startup_state=trial_disabled;;
    OBSERVE|SCRUB|SHADOW|BETA) ;;
    *) oneboot=unconfirmed; startup_state=trial_config_unconfirmed;;
esac
if [ -e "$BASE/logs/capture.stop" ] || [ -L "$BASE/logs/capture.stop" ]; then
    oneboot=unconfirmed
    startup_state=capture_stop_requested
fi
if [ "$guard_previous_armed_boot" = invalid ]; then
    oneboot=unconfirmed
    startup_state=prior_arming_history_invalid
fi
runtime_disable_next_start=absent
if [ -e "$BASE/logs/disable-next-start" ] || [ -L "$BASE/logs/disable-next-start" ]; then
    runtime_disable_next_start=present
    oneboot=unconfirmed
    startup_state=runtime_disabled_next_start
fi
echo "runtime_disable_next_start=$runtime_disable_next_start"
guard_config_binding=unconfirmed
if [ "$startup_state" = guard_committed_after_new_boot ]; then
    # Row 3 of the validated v3 manifest is the configuration digest used by
    # guard selection. A later file edit cannot turn an OBSERVE boot into an
    # apparent SHADOW boot merely by changing the current config text.
    if expected_config_digest=$(LC_ALL=C awk 'NR==3 {print; exit}' "$BASE/guard/consumed") &&
       actual_config_digest=$(hash "$BASE/mx5dr.conf"); then
        if [ "$expected_config_digest" = "$actual_config_digest" ]; then
            guard_config_binding=matched
        else
            guard_config_binding=changed
            startup_state=guard_config_changed_since_selection
        fi
    else
        startup_state=guard_config_unconfirmed
    fi
elif [ "$startup_state" = guard_committed_persistent ]; then
    # Row 6 of the persistent manifest is the same configuration digest.
    if expected_config_digest=$(LC_ALL=C awk 'NR==6 {print; exit}' "$BASE/guard/persist") &&
       actual_config_digest=$(hash "$BASE/mx5dr.conf"); then
        if [ "$expected_config_digest" = "$actual_config_digest" ]; then
            guard_config_binding=matched
        else
            guard_config_binding=changed
            oneboot=unconfirmed
            startup_state=guard_config_changed_since_selection
        fi
    else
        oneboot=unconfirmed
        startup_state=guard_config_unconfirmed
    fi
fi
echo "guard_config_binding=$guard_config_binding"
set --
retained=0
# Fixed bounded filenames only, oldest first within each stream. A rotated-away
# boot is unavailable evidence; do not attribute orphan rows to this boot.
for name in trace.2.jsonl trace.1.jsonl trace.0.jsonl collector.1.jsonl collector.0.jsonl \
            trace.storage.json collector.storage.json; do
    file=$BASE/logs/$name
    if [ -e "$file" ] || [ -L "$file" ]; then
        regular "$file"
        bytes=$(wc -c < "$file")
        case "$name" in *.storage.json) limit=1024;; collector.*) limit=4194304;; *) limit=41943040;; esac
        [ "$bytes" -le "$limit" ] || fail "Log exceeds expected per-file bound: $name"
        retained=$((retained + bytes))
        set -- "$@" "$file"
    fi
done
echo "one_boot=$oneboot retained_bytes=$retained trace_cap_bytes=125829120 collector_cap_bytes=8388608 (120+8 MiB, rotates)"
echo "guard_last_boot=$guard_last_boot guard_consumed=$guard_consumed; markers do not prove SM received the trial path or that runtime capture began."
echo "guard_arm=$guard_arm guard_armed_boot=$guard_armed_boot guard_previous_armed_boot=$guard_previous_armed_boot startup_state=$startup_state"
echo "guard_arm_schema=$guard_arm_schema guard_consumed_schema=$guard_consumed_schema (v2 is retained evidence, not a v3 startup gate)"
echo "config_mode=$config_mode"
case "$startup_state" in
    new_linux_boot_arm_unconsumed) echo 'New CMU Linux boot observed, but guard arm remains; selection and capture are not yet observed. Wait 60 seconds while parked and run menu 2 once more; if still incomplete, export with 3 and disarm with 4.';;
    guard_selected_same_boot_as_arm) echo 'Guard selected in the arming Linux boot: a new boot was not observed.';;
esac
echo 'Measured SHADOW rate (2026-10-05 drive) is about 28 KB/s on average and 36 KB/s with AA connected, so the 120 MiB trace can rotate out its oldest data after about 58 minutes. Park and export within about 50 minutes, at the first parked USB return, without reinstalling or rearming. Reboot may leave incomplete final rows; do not repeat a drive just to obtain a status pass.'
space_ok=0
space_free=$(storage_free_kib "$persist") || space_free=unknown
if [ "$space_free" != unknown ] && [ "$space_free" -gt 8256 ]; then space_ok=1; fi
echo "storage_available_kib=$space_free reserve_kib=8192 margin_kib=64"
# Parked evidence of which stock services run and whether each carries this
# package's preload. Reads /proc metadata only; never signals or restarts anything.
# stack_soft is the default thread stack in KiB-bytes the service inherited.
service_state() (
    svc=$1; token=$2; found=
    for dir in "$ROOT"/proc/[0-9]*; do
        [ -d "$dir" ] && [ ! -L "$dir" ] && [ -f "$dir/cmdline" ] || continue
        # Real CMU form: comm L_<svc>, argv "/jci/sm/sm_svclauncher -l <svc> <plugin> 0 -a".
        # Do not rely on comm; require the stock launcher as argv[0] and the service as a whole argument.
        args=$(tr '\000' ' ' < "$dir/cmdline" 2>/dev/null | head -c 512) || continue
        case "$args" in /jci/sm/sm_svclauncher\ *) ;; *) continue;; esac
        case "$args" in *" $svc "*) found=$dir; break;; esac
    done
    if [ -z "$found" ]; then echo "service_$svc=not_running"; exit 0; fi
    stack=$(awk '/^Max stack size/ {print $4}' "$found/limits" 2>/dev/null)
    uid=$(awk '/^Uid:/ {print $2}' "$found/status" 2>/dev/null)
    loaded=no
    if grep -q "/$token" "$found/maps" 2>/dev/null; then loaded=yes; fi
    echo "service_$svc=running pid=${found##*/} uid=${uid:-unknown} stack_soft_bytes=${stack:-unknown} package_preload=$loaded"
)
service_state jciAAPA libmx5dr.so
service_state jciLDS libmx5dr-ldstap.so
service_state jciVBS libmx5dr-vimtap.so
[ "$#" -gt 0 ] || fail 'No retained logs; current collection evidence unavailable'
# Take the snapshot time immediately before reading the growing journals; the
# service scan above can take seconds. Rows newer than this are ignored.
now=$(awk 'NR==1 && $1 ~ /^[0-9]+\.[0-9]+$/ {print $1}' "$ROOT/proc/uptime")
[ -n "$now" ] || fail 'Cannot read current monotonic uptime'
LC_ALL=C awk -v boot="$boot_id" -v now="$now" -v oneboot="$oneboot" \
    -v startup_state="$startup_state" -v space_ok="$space_ok" \
    -f "$HERE/trial_status.awk" "$@"
