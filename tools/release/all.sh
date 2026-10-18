#!/bin/sh
# Every non-publishing stage in order: build, zip (abort on failure), then
# arm, host, pack, stock, soak, replay (each runs and records its exit even
# when an earlier one failed). Never drafts or publishes. Start it as a
# tracked background job; follow it with status.sh.
#   sh tools/release/setup.sh TAG COMMIT && nohup sh tools/release/all.sh > /dev/null 2>&1 &
#   sh tools/release/status.sh
# REL_STAGES overrides the list after zip (default: arm host pack stock soak replay).
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
rel_work "${1:-}"
echo "$$" > "$WORK/jobs/all.pid"
sh "$REL_TOOLS/build.sh" "$WORK" || { echo 1 > "$WORK/jobs/all.exit"; exit 1; }
sh "$REL_TOOLS/zip.sh" "$WORK" || { echo 1 > "$WORK/jobs/all.exit"; exit 1; }
rc=0
for s in ${REL_STAGES:-arm host pack stock soak replay}; do
    sh "$REL_TOOLS/$s.sh" "$WORK" || rc=1
done
echo "$rc" > "$WORK/jobs/all.exit"
rm -f "$WORK/jobs/all.pid"
exit $rc
