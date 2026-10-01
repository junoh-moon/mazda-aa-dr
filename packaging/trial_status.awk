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
}
function clear_model_snapshot() {shadow=""; solution=""; processed=""; pipeline=""; result=""; attempts=""; intervals=""}
function reset_runtime() {
    health=""; hooks=""; dropped=""; audit=""; capture=""; mode=""
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
        mode=field("mode")
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
    }
    if (FILENAME ~ /\/trace\.[012]\.jsonl$/ && runtime) {
        if (kind=="shadow_boot") capture=field("capture_active")
        if (kind=="health") {
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
        if (kind=="position") {position=field("mono_ns"); position_mode=field("mode")}
        if (kind=="shadow") {
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
        if (kind=="poll") poll=field("end_ns")
        if (kind=="collector_stop") stopped=1
    }
}
END {
    report("storage_headroom",space_ok==1,"reserve=8MiB plus_write_margin")
    report("storage_capture_clean",!storage_stopped,"current_boot")
    report("guard_current_boot",oneboot=="consumed_this_boot",oneboot)
    report("runtime_current_boot",runtime, "mode=" mode)
    report("health_recent",runtime && health_recent(health),"window=5s")
    report("hooks",runtime && health_recent(health) && hooks=="true","")
    report("capture_active",runtime && health_recent(health) && mode=="4" && capture=="true","")
    report("wheels_received_recently",runtime && fresh(sensor[1]),"receipt_only")
    report("yaw_received_recently",runtime && fresh(sensor[2]),"receipt_only")
    report("reverse_received_recently",runtime && fresh(sensor[3]),"receipt_only_not_direction_quality")
    report("audit_clean",runtime && health_recent(health) && audit=="0" && dropped=="0", "audit_fault=" audit " dropped=" dropped)
    report("collector_poll_recent",collector && !stopped && poll_recent(poll),"window=8s polling_does_not_prove_sensor_validity")
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
    if (bad) print "Collection evidence incomplete. Keep/export existing logs; missing/rotated boot markers cannot be reconstructed by this check."
    else print "Capture startup evidence only. Inspect MODEL status and reasons; this is not a completed navigation trial."
    exit bad
}
