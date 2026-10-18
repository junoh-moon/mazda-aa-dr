#!/bin/sh
# Release stage 8 (G11): private replay byte identity against the previous
# release commit (tests/gates/g11_replay.sh). Needs MX5DR_REPLAY_CHAIN (private
# boot list) and G11_BASE_COMMIT (the previous release's tag commit); the
# experimental-off flags are G11_BASE_ARGS / G11_CAND_ARGS.
#   sh tools/release/replay.sh [WORK]
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
rel_work "${1:-}"
stage_begin replay
need MX5DR_REPLAY_CHAIN; need G11_BASE_COMMIT
cd "$SRC" || exit 1
G11_CAND_SRC=$SRC G11_MAX_LOAD=$REL_MAX_LOAD sh "$GATES/g11_replay.sh" "$WORK/gates/replay"
e=$?
# Only the counts and names leave the private work dir.
grep -E '^G11|^DIFF|^boots|exit' "$WORK/gates/replay/g11.log" > "$WORK/evidence/replay.txt"; evidence replay.txt
exit $e
