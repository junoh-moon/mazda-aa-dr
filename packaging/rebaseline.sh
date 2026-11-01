#!/bin/sh
# Re-register the installed persistent BETA product against the CURRENT
# /jci/sm/sm.conf and /jci/sm/sm_WCP.conf after another tool (for example
# mazda-cmu-tune) changed them. Contract: docs/INTEROP_KO.md (contract 1).
# Installed copy: /data_persist/mx5-aa-dr/tools/rebaseline.sh
#
#   sh rebaseline.sh            register  rc 0 done | 1 precheck_failed | 2 tripped | 3 partial
#   sh rebaseline.sh --check    read-only rc 0 match | 4 mismatch | 2 tripped | 1 unknown
#   sh rebaseline.sh --version  prints contract=1 (the only output without a result= line)
#
# The last stdout line of register and --check is exactly one result= line.
# Parked use only. Never restarts anything, never edits OEM files, never waits
# for a lock, never reads stdin, never clears a guard trip, a runtime disable
# request or an owner switch, and never authorizes payload or configuration
# bytes other than the ones menu 1 installed (checked against installed.txt).
# Only the two trial templates and their source pins are rebuilt, then the
# guard's own `enable` binds them (a fresh persist-state: the first boot after
# this is a probation boot, exactly as after menu 1).
CONTRACT=1
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ACTION=register
case "$#:${1:-}" in
    0:) ;;
    1:--check) ACTION=check;;
    1:--version) echo "contract=$CONTRACT"; exit 0;;
    *) echo 'mx5dr: usage: sh rebaseline.sh [--check|--version]' >&2
       echo 'result=precheck_failed:usage'; exit 1;;
esac

# The work runs in a child shell so that common.sh's exit trap (lock release,
# staged-file cleanup, remount restore) finishes before the result is printed.
# The child reports on fd 3: stage:<name>, reason:<code> and, on a decided
# outcome, final:<rc> <result line>. Its own exit status only says whether the
# cleanup succeeded after a decided outcome.
child() {
    . "$HERE/common.sh"
    exec < /dev/null
    stage() { printf 'stage:%s\n' "$1" >&3; }
    reason() { printf 'reason:%s\n' "$1" >&3; }
    final() { printf 'final:%s %s\n' "$1" "$2" >&3; exit 0; }
    clean_code() { printf '%s' "$1" | tr -c 'A-Za-z0-9_.:-' '_' | cut -c1-64; }
    guard_run() {
        guard_exec=$BASE/guard/mx5dr-guard
        if [ -n "$ROOT" ]; then guard_exec=${MX5DR_FIXTURE_GUARD:-$ROOT/missing-guard}; fi
        [ -f "$guard_exec" ] && [ -x "$guard_exec" ] || return 2
        MX5DR_GUARD_ROOT=$ROOT "$guard_exec" "$@"
    }
    field() { printf '%s\n' "$2" | awk -F= -v k="$1" '$1==k {sub(/^[^=]*=/, ""); print; exit}'; }
    stage pre
    reason storage
    validate_persist
    reason not_installed
    regular "$BASE/installed.txt"
    policy=$(field policy "$(cat "$BASE/installed.txt")")
    installed_mode=$(field mode "$(cat "$BASE/installed.txt")")
    reason unsupported_policy
    [ "$policy" = persistent ] && [ "$installed_mode" = BETA ] || fail "Only the persistent BETA installation is re-registered (installed: ${installed_mode:-?}/${policy:-?}); use menu 1"
    reason pending
    [ ! -e "$BASE/pending" ] || fail 'Incomplete installation transaction: use menu 4, then 1'
    reason guard
    status=$(guard_run status) || fail 'Guard status unavailable'
    persist=$(field persist "$status")
    tripped=$(field tripped "$status")
    verify=$(field verify "$status")
    # A trip outranks every mismatch: re-registering cannot re-enable it.
    if [ "$persist" = tripped ]; then
        final 2 "result=tripped:$(clean_code "$tripped")"
    fi
    # The guard reports a pin mismatch before a pending runtime stop request,
    # so look at the request itself: acknowledging it is menu 1's job.
    runtime_stop=no
    if [ -e "$BASE/logs/disable-next-start" ] || [ -L "$BASE/logs/disable-next-start" ]; then runtime_stop=yes; fi
    if [ "$ACTION" = check ]; then
        [ "$persist" = enabled ] || final 1 "result=unknown:not_enabled"
        [ "$runtime_stop" = no ] || final 1 "result=unknown:runtime_disabled"
        case "$verify" in
            ok) final 0 result=match;;
            baseline_edited:*|binding_changed:*) final 4 "result=mismatch:$(clean_code "$verify")";;
            *) final 1 "result=unknown:$(clean_code "${verify:-guard}")";;
        esac
    fi
    reason not_enabled
    [ "$persist" = enabled ] || fail 'BETA is not enabled (no valid persistent enablement); use menu 1'
    reason runtime_disabled
    [ "$runtime_stop" = no ] || fail 'The runtime asked to stop; use menu 3, then menu 1'
    case "$verify" in
        ok) final 0 result=done;;
        baseline_edited:*|binding_changed:*) ;;
        runtime_disabled_pending) reason runtime_disabled; fail 'The runtime asked to stop; use menu 3, then menu 1';;
        baseline_has_token:*) reason baseline_has_token; fail "${verify#*:} carries a product preload; use menu 4, then 1";;
        *) reason "guard_$(clean_code "${verify%%:*}")"; fail "Guard state $verify is not a configuration change; use menu 2";;
    esac
    reason locked
    ALLOW_REMOUNT=1
    [ -z "$ROOT" ] || ALLOW_REMOUNT=0 # Fixtures never remount the host.
    mount_rw "$persist"
    lock
    reason firmware
    verify_firmware
    reason baseline_has_token
    for name in sm.conf sm_WCP.conf; do
        regular "$ROOT/jci/sm/$name"
        ! grep -F "$TOKEN" "$ROOT/jci/sm/$name" >/dev/null || fail "$name carries the product preload; use menu 4, then 1"
        ! grep -F "$TAP_TOKEN" "$ROOT/jci/sm/$name" >/dev/null || fail "$name carries the VBS tap preload; use menu 4, then 1"
        ! grep -F "$LDS_TOKEN" "$ROOT/jci/sm/$name" >/dev/null || fail "$name carries the LDS tap preload; use menu 4, then 1"
    done
    # Re-bind only what menu 1 installed: the three preloads by the hashes in
    # installed.txt and the exact persistent configuration text.
    reason payload
    for pair in 'libmx5dr.so payload_sha256' 'libmx5dr-vimtap.so tap_sha256' 'libmx5dr-ldstap.so lds_tap_sha256'; do
        set -- $pair
        regular "$BASE/$1"
        want=$(field "$2" "$(cat "$BASE/installed.txt")")
        [ -n "$want" ] && [ "$(hash "$BASE/$1")" = "$want" ] || fail "$1 differs from the installation record; use menu 1"
    done
    reason config
    regular "$BASE/mx5dr.conf"
    printf 'mode=BETA\nmax_log_bytes=16777216\nmax_log_files=3\nsample_ms=1000\nlog_profile=persistent\n' > "$BASE/guard/conf.check.new.$$"
    cmp -s "$BASE/mx5dr.conf" "$BASE/guard/conf.check.new.$$" || fail 'mx5dr.conf differs from the persistent BETA configuration; use menu 1'
    rm -f "$BASE/guard/conf.check.new.$$"
    # Keep the installed phone-channel form: the templates carry the bds
    # arguments only if menu 1 prepared them (the guard still decides per boot).
    reason template
    regular "$BASE/guard/normal.trial"
    regular "$BASE/guard/wcp.trial"
    BT=0
    if grep -F -- "$BT_ARG" "$BASE/guard/normal.trial" >/dev/null; then BT=1; fi
    MODE=BETA
    for pair in 'sm.conf normal' 'sm_WCP.conf wcp'; do
        set -- $pair
        snapshot=$BASE/guard/$1.source.new.$$
        cp -p "$ROOT/jci/sm/$1" "$snapshot"
        trial_to "$snapshot" "$BASE/guard/$2.trial.new.$$"
        hash "$snapshot" > "$BASE/guard/$2.source.sha256.new.$$"
        chmod 0600 "$BASE/guard/$2.source.sha256.new.$$"
        rm -f "$snapshot"
    done
    reason concurrent_edit
    for pair in 'sm.conf normal' 'sm_WCP.conf wcp'; do
        set -- $pair
        [ "$(hash "$ROOT/jci/sm/$1")" = "$(cat "$BASE/guard/$2.source.sha256.new.$$")" ] || fail "Concurrent edit: $1"
    done
    # The guard's enable starts a fresh persist-state; keep the previous one
    # (and the pins it replaced) next to menu 1's evidence: numbered, last 3.
    reason evidence
    kept=$BASE/backups/persist-evidence
    [ ! -L "$BASE/backups" ] && [ ! -L "$kept" ] || fail 'Symlink evidence directory'
    mkdir -p "$kept"
    number=0
    for entry in "$kept"/[0-9]*; do
        name=${entry##*/}
        case "$name" in *[!0-9]*) continue;; esac
        [ -d "$entry" ] && [ ! -L "$entry" ] || continue
        [ "$name" -le "$number" ] || number=$name
    done
    number=$((number + 1))
    mkdir "$kept/$number"
    for item in "$BASE/guard/persist-state" "$BASE/guard/last-decision" \
        "$BASE/guard/normal.source.sha256" "$BASE/guard/wcp.source.sha256"; do
        if [ -e "$item" ] || [ -L "$item" ]; then regular "$item"; cp -p "$item" "$kept/$number/"; fi
    done
    printf 'rebaseline contract=%s\nverify_before=%s\nnormal_source=%s\nwcp_source=%s\n' "$CONTRACT" \
        "$(clean_code "$verify")" "$(cat "$BASE/guard/normal.source.sha256.new.$$")" \
        "$(cat "$BASE/guard/wcp.source.sha256.new.$$")" > "$kept/$number/rebaseline.txt"
    sync
    for entry in "$kept"/[0-9]*; do
        name=${entry##*/}
        case "$name" in *[!0-9]*) continue;; esac
        if [ -d "$entry" ] && [ ! -L "$entry" ] && [ "$name" -le $((number - 3)) ]; then rm -rf "$entry"; fi
    done
    # From here a failure leaves templates that no longer match the old
    # enablement: the guard then declines (binding_changed) and the CMU starts
    # stock until this command or menu 1 succeeds. Fail closed throughout.
    stage publish
    for trial in normal wcp; do
        mv -f "$BASE/guard/$trial.trial.new.$$" "$BASE/guard/$trial.trial"
        mv -f "$BASE/guard/$trial.source.sha256.new.$$" "$BASE/guard/$trial.source.sha256"
    done
    sync
    stage check
    guard_run check >/dev/null || fail 'Guard check failed after publishing the new templates'
    stage enable
    guard_run enable || fail 'Guard enable failed'
    stage verify
    after=$(guard_run verify) || fail "Guard verify after enable: $(field verify "$after")"
    [ "$(field verify "$after")" = ok ] || fail 'Guard verify after enable is not ok'
    echo "Re-registered BETA for the current sm.conf/sm_WCP.conf (evidence: backups/persist-evidence/$number). The first CMU start from now on is a probation boot. No restart."
    final 0 result=done
}

exec 4>&1
channel=$( (child) 3>&1 1>&4 ) && child_rc=0 || child_rc=$?
exec 4>&-
last() { printf '%s\n' "$channel" | awk -v p="$1:" 'index($0, p)==1 {v=substr($0, length(p)+1)} END {print v}'; }
decided=$(last final)
last_stage=$(last stage)
last_reason=$(last reason)
if [ -n "$decided" ] && [ "$child_rc" = 0 ]; then
    rc=${decided%% *}
    printf '%s\n' "${decided#* }"
    exit "$rc"
fi
if [ "$ACTION" = check ]; then
    echo "result=unknown:${last_reason:-error}"
    exit 1
fi
case "$last_stage" in
    ''|pre) echo "result=precheck_failed:${last_reason:-error}"; exit 1;;
    *) [ -z "$decided" ] || last_stage=cleanup
       echo "result=partial:$last_stage"; exit 3;;
esac
