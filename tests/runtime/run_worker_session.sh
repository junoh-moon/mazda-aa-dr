#!/bin/sh
# Run one authored worker-session scenario. Exit 77 from the test means the
# shared host stalled the worker past the motion age limit (250 ms fresh,
# 2 s for contiguous late arrivals since 2026-10-06; see
# validation/WORKER_STALL_STALE_2026-10-06.md); such a run cannot check
# the lifecycle. Retry it a bounded number of times and fail if every attempt
# is inconclusive. Any other failure fails immediately.
attempts=3
n=1
while :; do
    "$@"
    result=$?
    [ "$result" -eq 77 ] || exit "$result"
    if [ "$n" -ge "$attempts" ]; then
        echo "worker session: $attempts consecutive inconclusive host-stall runs: $*" >&2
        exit 1
    fi
    echo "worker session: inconclusive host stall, retry $n/$((attempts - 1)): $*" >&2
    n=$((n + 1))
done
