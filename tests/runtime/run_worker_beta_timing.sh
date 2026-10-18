#!/bin/sh
# Run one test_worker_beta scenario whose journal-writer/worker timing check
# (no beta_journal_lag "lagging" row, beta_summary rows at most 1.6 s apart)
# depends on the host giving those threads CPU (g3_cancel, g3_garage; see
# validation/G3_GARAGE_EXIT_FIX_2026-10-17.md, third fix round). Exit 77
# from the test means every logic assertion passed and only that timing
# check failed. Retry it at most 2 more times, logging the host load of each
# attempt, and fail if every attempt is inconclusive. Any other failure fails
# immediately. Thresholds are not changed (same philosophy as
# tests/run_arm_timing.sh).
attempts=3
n=1
while :; do
    load=$(cut -d' ' -f1-3 /proc/loadavg 2>/dev/null || echo -)
    "$@"
    result=$?
    [ "$result" -eq 77 ] || exit "$result"
    if [ "$n" -ge "$attempts" ]; then
        echo "WORKER_BETA_TIMING_FAILED attempts=$attempts load=$load: $*" >&2
        exit 1
    fi
    echo "WORKER_BETA_TIMING_RETRY $n/$((attempts - 1)) load_at_start=$load load_now=$(cut -d' ' -f1-3 /proc/loadavg 2>/dev/null || echo -): $*" >&2
    n=$((n + 1))
    sleep 5
done
