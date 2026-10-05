# Narrow parser for the emitted compact journal schema, not general JSON.
# No strings from logs are executed. Unknown/incomplete records provide no proof.
function field(key, s, p) {
    s=$0; p="\"" key "\":"
    if (!index(s,p)) return ""
    s=substr(s,index(s,p)+length(p))
    if (substr(s,1,1)=="\"") {s=substr(s,2); sub(/".*/,"",s)}
    else sub(/[,}].*/,"",s)
    return s
}
# /proc/uptime truncates to centiseconds; CLOCK_MONOTONIC journal timestamps
# can consequently be <10 ms ahead. This allowance is only for this diagnostic,
# never sensor qualification/core freshness. Bound heartbeat evidence separately
# from receipt observations: a silently failed journal writes no terminal row.
function not_future(ns) {return ns ~ /^[0-9]+$/ && ns+0>0 && ns+0<=now*1e9+10000000}
function recent(ns, seconds) {return not_future(ns) && now-ns/1e9<=seconds}
function fresh(ns) {return recent(ns,30)}
function health_recent(ns) {return recent(ns,5)}
function poll_recent(ns) {return recent(ns,8)}
function model_recent(ns) {return recent(ns,2)}
function after_now(ns) {return ns ~ /^[0-9]+$/ && ns+0>now*1e9+10000000}
# Whole seconds between a row time and `now`, or "none" for no usable row.
function age(ns, a) {
    if (!not_future(ns)) return "none"
    a=now-ns/1e9; if (a<0) a=0
    return sprintf("%d",a)
}
function unsigned_field(key) {return $0 ~ ("\"" key "\":[0-9]+[,}]")}
function valid_boot_id(id, compact) {
    if (length(id)!=36 || substr(id,9,1)!="-" || substr(id,14,1)!="-" ||
        substr(id,19,1)!="-" || substr(id,24,1)!="-") return 0
    compact=id; gsub(/-/,"",compact)
    return length(compact)==32 && compact ~ /^[0-9a-f]+$/
}
function reset_retained(id) {
    retained_boot=id; retained_positions=0; retained_motion=0; retained_end=0
    retained_model=0; retained_valid=0; retained_attempts=0
    # BETA rows of the last retained boot (same-boot worker restarts add up).
    beta_requested=0; beta_boot_enabled=""; beta_boot_reason=""; beta_fence=""
    beta_session_hooks=""; beta_install=""; beta_enable=""; beta_hold_seen=0
    beta_engaged=0; beta_replaced=0; beta_replaced_nonzero=0; beta_hold=0
    beta_withdrawals=0; beta_withdraw_reason="none"; beta_last_state=""; beta_last_reason=""
    beta_class_seen=0; beta_no_fix=0
}
# Member of the boot row's flat "beta":{...} object (no nested objects).
function beta_member(key, s, p) {
    s=$0; p=index(s,"\"beta\":{")
    if (!p) return ""
    s=substr(s,p+8); sub(/}.*/,"",s)
    p=index(s,"\"" key "\":")
    if (!p) return ""
    s=substr(s,p+length(key)+3)
    if (substr(s,1,1)=="\"") {s=substr(s,2); sub(/".*/,"",s)}
    else sub(/,.*/,"",s)
    return s
}
function clear_model_snapshot() {shadow=""; solution=""; processed=""; pipeline=""; result=""; attempts=""; intervals=""}
function reset_runtime() {
    health=""; hooks=""; boot_install=""; dropped=""; audit=""; capture=""; mode=""
    computation=""; position=""; position_mode=""; anchor="none_observed"
    rejected_raw=0; untimed_rejected_raw=0; untimed_motion_reset=0
    clear_model_snapshot()
    position_rejection="none_observed"; motion_rejection="none_observed"
    motion_exclusion="none_observed"
    pipeline_reset="none_observed"; reset_operation=""; reset_sequence=""; reset_time=""
    untimed_pipeline_reset=0
    for (i=1;i<=3;i++) {sensor[i]=""; rejected_sensor[i]=0}
}
function report(name, ok, detail) {
    print name "=" (ok ? "observed" : "unavailable") (detail!="" ? " " detail : "")
    if (!ok) bad=1
}
function observe(name, ok, detail) {
    print name "=" (ok ? "observed" : "unavailable") (detail!="" ? " " detail : "")
}
BEGIN {reset_runtime(); reset_retained(""); bad=0}
# Files must contain complete emitted object lines before they count as evidence.
!/^\{.*\}$/ {next}
FILENAME ~ /\/trace\.[012]\.jsonl$/ && !/^\{"kind":"[a-z_]+",/ {next}
FILENAME ~ /\/collector\.[01]\.jsonl$/ && !/^\{"stream":"collector","collector_pid":[0-9]+,"observed_at_mono_ns":[0-9]+,"producer_mono_ns":null,"producer_time_status":"unknown","kind":"[a-z_]+",/ {next}
{
    kind=field("kind")
    # Journals keep growing while this check runs. A row stamped after the
    # snapshot time did not exist at `now`: skip it entirely, including
    # terminal rows (capture_end, collector_stop) and later health/poll rows.
    # Only this kernel boot shares the `now` clock; earlier boots keep their
    # own (possibly larger) monotonic times and stay retained evidence.
    if (FILENAME ~ /\/trace\.[012]\.jsonl$/ && after_now(field("mono_ns")) &&
        (kind=="boot" ? field("boot_id")==boot : runtime)) next
    if (FILENAME ~ /\/collector\.[01]\.jsonl$/ && after_now(field("observed_at_mono_ns")) &&
        (kind=="collector_boot" ? field("boot_id")==boot : collector)) next
    if (kind=="storage_stop" && FILENAME ~ /\/(trace|collector)\.storage\.json$/ &&
        field("boot_id")==boot && not_future(field("mono_ns"))) {
        print "storage_stop=" field("stream") " reason=" field("reason") " available_bytes=" field("available_bytes") " mono_ns=" field("mono_ns")
        if(field("stream")=="trace" && field("mono_ns")+0>=runtime_boot+0) {capture="false"; computation="false"}
        if(field("stream")=="collector" && field("mono_ns")+0>=collector_start+0) stopped=1
        storage_stopped=1; bad=1; next
    }
    if (kind=="boot" && FILENAME ~ /\/trace\.[012]\.jsonl$/) {
        # Keep only the last boot's retained rows, without comparing its clock
        # to this boot's uptime. Same-boot worker restarts reset current health
        # below but do not erase earlier records from that boot. A missing or
        # malformed boot marker never assigns orphan rows to a known boot.
        identity=field("boot_id")
        if (!valid_boot_id(identity)) reset_retained("")
        else if (identity!=retained_boot) reset_retained(identity)
        reset_runtime(); runtime=(field("boot_id")==boot); runtime_boot=field("mono_ns")
        if (!not_future(runtime_boot)) runtime=0
        mode=field("mode"); boot_install=field("install")
        if (retained_boot!="") {
            # The latest boot row of the retained boot owns these fields; a
            # restarted worker journals its own enable decision again.
            beta_requested=(mode=="5"); beta_install=field("install")
            beta_boot_enabled=beta_member("enabled"); beta_boot_reason=beta_member("reason")
            beta_fence=beta_member("session_fence"); beta_session_hooks=field("session_hooks")
            beta_enable=""; beta_hold_seen=0
        }
        next
    }
    if (kind=="collector_boot" && FILENAME ~ /\/collector\.[01]\.jsonl$/) {
        collector=(field("boot_id")==boot); collector_start=field("observed_at_mono_ns")
        poll=""; stopped=0; next
    }
    if (FILENAME ~ /\/trace\.[012]\.jsonl$/ && retained_boot!="") {
        if (kind=="position") retained_positions++
        if (kind=="motion_batch" && /"schema":1[,}]/) retained_motion++
        if (kind=="capture_end" && field("boot_id")==retained_boot) retained_end=1
        # These are row counts, not drain/integration totals or a trial verdict.
        # Reset/teardown does not erase prior rows; malformed metadata adds no
        # calculation evidence. Physical measurement time remains unknown.
        if (kind=="shadow" && field("domain")=="model" && /"assist_ready":false[,}]/ &&
            /"model_valid":(true|false)[,}]/ && unsigned_field("events") &&
            field("result") ~ /^[A-Z_]+$/ && field("pipeline") ~ /^[A-Z_]+$/) {
            retained_model++
            if (field("model_valid")=="true") retained_valid++
            if (unsigned_field("drain_calls_total") && field("drain_calls_total")+0>0)
                retained_attempts++
        }
        # BETA journal rows record decisions; they are not phone acceptance.
        if (kind=="beta_state" && field("domain")=="beta" && field("to") ~ /^[A-Z_]+$/) {
            if (field("from")=="DISABLED" && beta_enable=="")
                beta_enable=(field("to")=="ARMED" ? "armed" : "disabled:" field("reason"))
            if (field("to")=="ENGAGED") beta_engaged++
            if (field("to")=="WITHDRAWN") {beta_withdrawals++; beta_withdraw_reason=field("reason")}
            beta_last_state=field("to"); beta_last_reason=field("reason")
            # Optional newer field; tolerated, never required.
            if (field("position_class") ~ /^[A-Z_]+$/) {
                beta_class_seen=1
                if (field("position_class")=="NO_FIX") beta_no_fix++
            }
        }
        if (kind=="beta_hold" && field("domain")=="beta" && field("event")=="hold_set" &&
            unsigned_field("count") && field("count")+0>beta_hold_seen) {
            beta_hold+=field("count")-beta_hold_seen; beta_hold_seen=field("count")+0
        }
        if (kind=="send" && /"choice":3,/) {
            beta_replaced++
            if (field("result")!="0") beta_replaced_nonzero++
        }
    }
    if (FILENAME ~ /\/trace\.[012]\.jsonl$/ && runtime) {
        if (kind=="shadow_boot") capture=field("capture_active")
        # The status snapshot is taken at `now`. The journals keep growing
        # while this script runs; a row written after `now` must not replace
        # the latest value that existed at `now` (it would then fail the age
        # check and hide a live runtime).
        if (kind=="health" && not_future(field("mono_ns"))) {
            health=field("mono_ns"); hooks=field("hook_installed")
            dropped=field("dropped"); audit=field("audit_fault")
            if (field("capture_active")!="") capture=field("capture_active")
            computation=field("computation_active")
        }
        if (kind=="capture_end" || kind=="capture_incomplete") {
            capture="false"; computation="false"; clear_model_snapshot(); anchor="none_observed"
        }
        if (kind=="shadow_disabled") {
            computation="false"; clear_model_snapshot(); anchor="none_observed"
        }
        if ((kind=="shadow_session" || kind=="shadow_bus") && field("reset")=="true") {
            clear_model_snapshot(); anchor="none_observed"
        }
        if (kind=="shadow_input_reset" || kind=="shadow_pipeline_reset") {
            clear_model_snapshot(); anchor="none_observed"
        }
        if (kind=="shadow_pipeline_reset") {
            if (not_future(field("mono_ns"))) {
                pipeline_reset=field("reason"); reset_operation=field("operation")
                reset_sequence=field("receive_seq"); reset_time=field("mono_ns")
            } else if (field("mono_ns")=="0") untimed_pipeline_reset=1
        }
        if (kind=="position" && not_future(field("mono_ns"))) {position=field("mono_ns"); position_mode=field("mode")}
        if (kind=="shadow" && not_future(field("mono_ns"))) {
            # events is cumulative queue insertions (including POSITION), not
            # the number of drained or motion inputs. A later unusable record
            # must not resurrect an earlier usable MODEL snapshot.
            clear_model_snapshot()
            if (field("domain")=="model" && field("assist_ready")=="false" &&
                field("model_valid") ~ /^(true|false)$/ && unsigned_field("events") &&
                field("result") ~ /^[A-Z_]+$/ && field("pipeline") ~ /^[A-Z_]+$/) {
                shadow=field("mono_ns"); processed=field("events")
                solution=field("model_valid"); pipeline=field("pipeline"); result=field("result")
                if (unsigned_field("drain_calls_total")) attempts=field("drain_calls_total")
                if (unsigned_field("intervals")) intervals=field("intervals")
            }
        }
        if (fresh(field("mono_ns"))) {
            if (kind=="shadow_calibration") anchor=field("gps_anchor_gate")
            if (kind=="shadow_position_rejected") position_rejection=field("reason")
            if (kind=="shadow_input_reset") motion_rejection=field("reason")
            if (kind=="shadow_motion_excluded") motion_exclusion=field("reason")
        }
        if (kind=="shadow_input_reset" && field("mono_ns")=="0") untimed_motion_reset=1
        # Rejected datagrams are failure evidence, never accepted capture.
        # checked_ns is worker time and cannot refresh a sensor's receipt.
        if (kind=="motion_rejected" && field("authenticated_decoded")=="true" &&
            field("sensor") ~ /^[123]$/) {
            rejected_raw=1
            if (field("checked_ns")=="0") untimed_rejected_raw=1
            if (fresh(field("checked_ns")) && field("checked_ns")+0>=runtime_boot+0)
                rejected_sensor[field("sensor")]=1
        }
        if (kind=="motion_batch" && field("schema")=="1") {
            rows=$0
            sub(/^.*"events":\[\[/,"",rows); sub(/\]\]\}$/, "", rows)
            count=split(rows,events,/\],\[/)
            for (j=1;j<=count;j++) {
                n=split(events[j],values,",")
                if (n==10 && values[1] ~ /^[123]$/ && fresh(values[3]) && values[3]+0>=runtime_boot+0)
                    sensor[values[1]]=values[3]
            }
        }
    }
    if (FILENAME ~ /\/collector\.[01]\.jsonl$/ && collector) {
        if (kind=="poll" && not_future(field("end_ns"))) poll=field("end_ns")
        if (kind=="collector_stop") stopped=1
    }
}
END {
    report("storage_headroom",space_ok==1,"reserve=8MiB plus_write_margin")
    report("storage_capture_clean",!storage_stopped,"current_boot")
    report("guard_current_boot",oneboot=="consumed_this_boot",oneboot)
    report("linux_reboot_after_arm",startup_state=="guard_committed_after_new_boot",startup_state)
    report("runtime_current_boot",runtime, "mode=" mode)
    report("health_recent",runtime && health_recent(health),"window=5s health_age_s=" age(health))
    # The boot row records whether the AA hook was installed; a stale or
    # missing health row is a separate (health_recent) question.
    report("hooks",runtime && boot_install=="ok" && hooks!="false","install=" (boot_install=="" ? "missing" : boot_install) " health_hook_installed=" (hooks=="" ? "unknown" : hooks))
    report("capture_active",runtime && health_recent(health) && (mode=="4" || mode=="5") && capture=="true","")
    report("wheels_received_recently",runtime && fresh(sensor[1]),"receipt_only")
    report("yaw_received_recently",runtime && fresh(sensor[2]),"receipt_only")
    report("reverse_received_recently",runtime && fresh(sensor[3]),"receipt_only_not_direction_quality")
    report("audit_clean",runtime && health_recent(health) && audit=="0" && dropped=="0", "audit_fault=" audit " dropped=" dropped)
    report("collector_poll_recent",collector && !stopped && poll_recent(poll),"window=8s poll_age_s=" age(poll) " polling_does_not_prove_sensor_validity")
    # A parked capture may collect raw even when AA/session/GPS is unavailable.
    # Expose MODEL observations without changing the collection-only exit code.
    position_recent=runtime && fresh(position)
    observe("oem_position_recent",position_recent,position_recent?"mode=" position_mode:"")
    observe("computation_active",runtime && health_recent(health) && computation=="true","")
    diagnostic_recent=runtime && model_recent(shadow)
    diagnostic_detail=""
    if (diagnostic_recent)
        diagnostic_detail="events_queued_total=" processed " pipeline=" pipeline " result=" result
    observe("model_diagnostic_recent",diagnostic_recent,diagnostic_detail)
    # This is an actual drain invocation counter, including empty/waiting
    # calls, not queued input count or successful integration/qualification.
    observe("calculation_attempt_recent",diagnostic_recent && health_recent(health) &&
        computation=="true" && attempts+0>0,
        "drain_calls_total=" (attempts==""?"unknown":attempts) " intervals_total=" (intervals==""?"unknown":intervals))
    observe("wheels_rejected_checked_recently",runtime && rejected_sensor[1],"worker_check_only")
    observe("yaw_rejected_checked_recently",runtime && rejected_sensor[2],"worker_check_only")
    observe("reverse_rejected_checked_recently",runtime && rejected_sensor[3],"worker_check_only")
    # A parked startup can legitimately await movement/GPS anchors. Expose
    # that separately instead of requiring a moving solution to start capture.
    usable=runtime && health_recent(health) && computation=="true" && model_recent(shadow) &&
        solution=="true" && result=="OK" && pipeline=="OK" && audit=="0" && dropped=="0"
    print "model_solution=" (usable ? "observed" : "not_observed") " domain=model assist_ready=false"
    print "gps_anchor_gate=" anchor " last_position_rejection_30s=" position_rejection " last_motion_reset_30s=" motion_rejection " last_model_exclusion_30s=" motion_exclusion
    print "rejected_raw_seen_this_boot=" (rejected_raw ? "true" : "false") " untimed_rejected_raw_seen=" (untimed_rejected_raw ? "true" : "false") " untimed_motion_reset_seen=" (untimed_motion_reset ? "true" : "false")
    print "last_pipeline_reset_this_boot=" pipeline_reset " operation=" reset_operation " receive_seq=" reset_sequence " mono_ns=" reset_time " untimed_reset_seen=" (untimed_pipeline_reset ? "true" : "false")
    print "retained_runtime_last_boot=" (retained_boot=="" ? "unavailable" : (retained_boot==boot ? "current_boot" : "previous_boot")) " boot_id=" (retained_boot=="" ? "unknown" : retained_boot) " position_records=" retained_positions " motion_batches=" retained_motion " capture_end_record=" (retained_end ? "observed" : "not_observed") " records_only_not_current_readiness"
    print "retained_model_diagnostic_records=" retained_model " model_valid_records=" retained_valid " drain_attempt_records=" retained_attempts " domain=model assist_ready=false records_only_not_trial_success"
    beta_scope=(retained_boot=="" ? "unavailable" : (retained_boot==boot ? "current_boot" : "previous_boot"))
    print "beta_requested=" (beta_requested ? "true" : "false") " beta_scope=" beta_scope " records_only_not_phone_acceptance"
    if (beta_requested) {
        fence=beta_fence
        if (fence=="declined_send_storage_counter") fence="declined"
        else if (fence=="observed_session_and_send_storage_counter") fence="observed"
        else if (fence=="") fence="missing"
        enabled=beta_boot_enabled; if (enabled=="") enabled="missing"
        reason=beta_boot_reason; if (reason=="") reason="missing"
        hook="not_installed"; if (beta_install=="ok") hook="installed"
        enable=beta_enable; if (enable=="") enable="none_observed"
        last_state=beta_last_state; if (last_state=="") last_state="none"
        last_reason=beta_last_reason; if (last_reason=="") last_reason="none"
        printf "beta_boot_enabled=%s beta_boot_reason=%s beta_hook=%s beta_install=%s beta_session_fence=%s beta_session_hooks=%s\n", enabled, reason, hook, beta_install, fence, beta_session_hooks
        printf "beta_enable=%s beta_last_state=%s beta_last_reason=%s\n", enable, last_state, last_reason
        printf "beta_engaged=%d beta_replaced_sends=%d beta_replaced_nonzero=%d beta_hold_set=%d beta_withdrawals=%d beta_last_withdraw_reason=%s\n", beta_engaged, beta_replaced, beta_replaced_nonzero, beta_hold, beta_withdrawals, beta_withdraw_reason
        if (beta_class_seen) printf "BETA NO_FIX: %d state rows\n", beta_no_fix
        printf "BETA: engaged %d times, replaced %d sends, hold %d, last state %s (%s), scope %s\n", beta_engaged, beta_replaced, beta_hold, last_state, last_reason, beta_scope
    }
    if (bad) print "Collection evidence incomplete. Keep/export existing logs; missing/rotated boot markers cannot be reconstructed by this check."
    else print "Capture startup evidence only. Inspect MODEL status and reasons; this is not a completed navigation trial."
    exit bad
}
