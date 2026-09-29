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
# never sensor qualification/core freshness. The 30-second age limit is unchanged.
function not_future(ns) {return ns ~ /^[0-9]+$/ && ns+0>0 && ns+0<=now*1e9+10000000}
function fresh(ns) {return not_future(ns) && now-ns/1e9<=30}
function reset_runtime() {
    health=""; hooks=""; dropped=""; audit=""; capture=""; mode=""
    for (i=1;i<=3;i++) sensor[i]=""
}
function report(name, ok, detail) {
    print name "=" (ok ? "observed" : "unavailable") (detail!="" ? " " detail : "")
    if (!ok) bad=1
}
BEGIN {reset_runtime(); bad=0}
# Files must contain complete emitted object lines before they count as evidence.
!/^\{.*\}$/ {next}
FILENAME ~ /\/trace\.[012]\.jsonl$/ && !/^\{"kind":"[a-z_]+",/ {next}
FILENAME ~ /\/collector\.[01]\.jsonl$/ && !/^\{"stream":"collector","collector_pid":[0-9]+,"observed_at_mono_ns":[0-9]+,"producer_mono_ns":null,"producer_time_status":"unknown","kind":"[a-z_]+",/ {next}
{
    kind=field("kind")
    if (kind=="boot" && FILENAME ~ /\/trace\.[012]\.jsonl$/) {
        reset_runtime(); runtime=(field("boot_id")==boot); runtime_boot=field("mono_ns")
        if (!not_future(runtime_boot)) runtime=0
        mode=field("mode")
        next
    }
    if (kind=="collector_boot" && FILENAME ~ /\/collector\.[01]\.jsonl$/) {
        collector=(field("boot_id")==boot); poll=""; stopped=0; next
    }
    if (FILENAME ~ /\/trace\.[012]\.jsonl$/ && runtime) {
        if (kind=="shadow_boot") capture=field("capture_active")
        if (kind=="health") {
            health=field("mono_ns"); hooks=field("hook_installed")
            dropped=field("dropped"); audit=field("audit_fault")
            if (field("capture_active")!="") capture=field("capture_active")
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
    report("guard_current_boot",oneboot=="consumed_this_boot",oneboot)
    report("runtime_current_boot",runtime, "mode=" mode)
    report("health_recent",runtime && fresh(health),"window=30s")
    report("hooks",runtime && fresh(health) && hooks=="true","")
    report("capture_active",runtime && fresh(health) && mode=="4" && capture=="true","")
    report("wheels_received_recently",runtime && fresh(sensor[1]),"receipt_only")
    report("yaw_received_recently",runtime && fresh(sensor[2]),"receipt_only")
    report("reverse_received_recently",runtime && fresh(sensor[3]),"receipt_only_not_direction_quality")
    report("audit_clean",runtime && fresh(health) && audit=="0" && dropped=="0", "audit_fault=" audit " dropped=" dropped)
    report("collector_poll_recent",collector && !stopped && fresh(poll),"polling_does_not_prove_sensor_validity")
    if (bad) print "Collection evidence incomplete. Keep/export existing logs; missing/rotated boot markers cannot be reconstructed by this check."
    else print "Current collection evidence observed; navigation accuracy, recovery and phone acceptance remain unverified."
    exit bad
}
