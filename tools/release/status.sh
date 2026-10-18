#!/bin/sh
# Print every stage's state (running/exit) and the evidence index of WORK.
#   sh tools/release/status.sh [WORK]
. "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)/common.sh"
rel_work "${1:-}"
echo "work $WORK  tag $TAG  commit $COMMIT"
for s in setup build zip arm host pack stock soak replay draft publish verify all; do
    if [ -f "$WORK/jobs/$s.exit" ]; then echo "$s: exit $(cat "$WORK/jobs/$s.exit")"
    elif [ -f "$WORK/jobs/$s.start" ]; then echo "$s: running or killed ($(cat "$WORK/jobs/$s.start")), last: $(tail -1 "$WORK/jobs/$s.log" 2>/dev/null | cut -c1-120)"
    fi
done
[ -f "$WORK/evidence/INDEX" ] && { echo '-- evidence'; cat "$WORK/evidence/INDEX"; }
exit 0
