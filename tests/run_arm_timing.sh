#!/bin/sh
# Run one timing-class ARM test (QEMU user mode) with a bounded retry.
# Usage: sh tests/run_arm_timing.sh <name> <command> [args...]
#
# Timing-class tests assert real product timing contracts (journal writer
# age flush, 1.5 s lag guard, worker turn and idle-gate deadlines) against
# the wall clock. Under qemu-arm on a shared host a guest thread can lose
# hundreds of milliseconds to host scheduling, so one run can fail without
# a product defect (validation/ARM_TIMING_STABILISATION_2026-10-10.md).
# The thresholds are not changed here. Instead:
#   - up to MX5DR_TIMING_ATTEMPTS (default 3) attempts; the test passes if
#     any attempt passes, and fails if every attempt fails, so a
#     deterministic failure still fails the suite;
#   - every attempt is reported with its exit code and its failing assertion
#     line (ARM_TIMING_ATTEMPT ...), and the full output of every attempt is
#     kept in the log (the first failure stays as evidence);
#   - attempts are MX5DR_TIMING_RETRY_PAUSE (default 5) seconds apart, and
#     each attempt line records the host load average at its start;
#   - a test that needed more than one attempt, or failed every attempt, is
#     appended to MX5DR_TIMING_LEDGER for the flaky report printed by
#     tests/run_arm_all.sh (ARM_TIMING_RETRIES=n).
# Exit 77 (a test's own "inconclusive host stall", as in
# tests/runtime/run_worker_session.sh) counts as a failed attempt.
# Only tests listed as timing class in tests/run_arm_all.sh use this
# wrapper; every other ARM test runs once.
[ "$#" -ge 2 ] || { echo "usage: $0 <name> <command> [args...]" >&2; exit 2; }
name=$1
shift
attempts=${MX5DR_TIMING_ATTEMPTS:-3}
case $attempts in ''|*[!0-9]*) echo "run_arm_timing: invalid MX5DR_TIMING_ATTEMPTS" >&2; exit 2 ;; esac
[ "$attempts" -ge 1 ] || attempts=1
log=$(mktemp "${TMPDIR:-/tmp}/mx5dr-timing.XXXXXX") || exit 1
trap 'rm -f "$log"' EXIT
# The failing assertion, or the last output line when there is none.
reason() {
    line=$(grep -E "Assertion .* failed|inconclusive|Terminated|Alarm clock|uncaught target signal" "$log" | head -n 1)
    [ -n "$line" ] || line=$(tail -n 1 "$log")
    [ -n "$line" ] || line='(no output)'
    printf '%s' "$line" | tr '\t"' ' '"'"
}
# Host load at the start of each attempt (Linux; "-" elsewhere): a timing
# failure under heavy load is expected, one on an idle host deserves a look.
load() { cut -d' ' -f1 /proc/loadavg 2>/dev/null || echo -; }
first=
first_load=
n=1
while :; do
    result=0
    started=$(load)
    "$@" >"$log" 2>&1 || result=$?
    cat "$log"
    if [ "$result" -eq 0 ]; then
        echo "ARM_TIMING_ATTEMPT name=\"$name\" attempt=$n/$attempts exit=0 load=$started"
        if [ "$n" -gt 1 ] && [ -n "${MX5DR_TIMING_LEDGER:-}" ]; then
            printf 'RETRIED\t%s\t%s\t%s\t%s\n' "$name" "$n" "$first_load" "$first" >>"$MX5DR_TIMING_LEDGER"
        fi
        exit 0
    fi
    why=$(reason)
    [ -n "$first" ] || { first=$why; first_load=$started; }
    echo "ARM_TIMING_ATTEMPT name=\"$name\" attempt=$n/$attempts exit=$result load=$started assertion=\"$why\""
    if [ "$n" -ge "$attempts" ]; then
        [ -z "${MX5DR_TIMING_LEDGER:-}" ] ||
            printf 'FAILED\t%s\t%s\t%s\t%s\n' "$name" "$n" "$first_load" "$first" >>"$MX5DR_TIMING_LEDGER"
        echo "ARM_TIMING_FAIL name=\"$name\": all $attempts attempts failed" >&2
        [ "$result" -ne 77 ] || result=1
        exit "$result"
    fi
    # A short pause lets a transient host burst pass before the next attempt.
    sleep "${MX5DR_TIMING_RETRY_PAUSE:-5}"
    n=$((n + 1))
done
