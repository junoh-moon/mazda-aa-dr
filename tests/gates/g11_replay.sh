#!/bin/sh
# G11 replay byte identity: replay every private boot with the experimental
# features OFF through the candidate's replay_beta and through the previous
# release's replay_beta (built from that commit in scratch), with one G3 store
# chained across the boots, and compare every output file.
#
#   MX5DR_REPLAY_CHAIN=/private/chain.txt sh tests/gates/g11_replay.sh [WORK]
#
# MX5DR_REPLAY_CHAIN   private list, one boot per line: "<logs dir> <boot id>",
#                      in drive order (the logs dir holds trace.N.jsonl and the
#                      collector journal of the exported vehicle tree). It is
#                      never copied into the repository; outputs stay in WORK.
# G11_BASE_COMMIT      previous release commit (default: v1.0.0-beta.17,
#                      2a154eeb6ce629760c17ea6ec31cdc00929e131a)
# G11_COMMON_ARGS      replay flags for both (default: --real-only --g3 on --g3-send on)
# G11_BASE_ARGS        experimental-off flags for the baseline (default: --g3-garage off)
# G11_CAND_ARGS        experimental-off flags for the candidate (default: --g3-garage off)
# G11_CAND_SRC         candidate source tree (default: this checkout)
# G11_MAX_LOAD         wait while the 1-min load is above this (default 4)
# Per boot: report .json, .csv, BETA .jsonl, .g3.jsonl, .sends.jsonl, .stdout,
# .stderr (a boot without position rows writes only stdout/stderr). The G3
# store thread timing fields (lat_ms, start_ns, end_ns) are normalised in every
# file (as in the beta.17 110/110 comparison); nothing else.
# Output: "G11: X/N files identical" and the differing names. Exit 0 when all
# are identical, 1 otherwise, 77 when the private chain is not available.
set -u
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/../.." && pwd)
chain=${MX5DR_REPLAY_CHAIN:-}
if [ -z "$chain" ] || [ ! -s "$chain" ]; then echo 'SKIP: MX5DR_REPLAY_CHAIN unset or empty (private boot list)'; exit 77; fi
work=${1:-$(mktemp -d "${TMPDIR:-/tmp}/mx5dr-g11.XXXXXX")}
mkdir -p "$work" || exit 1
work=$(CDPATH= cd -- "$work" && pwd)
base_commit=${G11_BASE_COMMIT:-2a154eeb6ce629760c17ea6ec31cdc00929e131a}
common=${G11_COMMON_ARGS:---real-only --g3 on --g3-send on}
base_args=${G11_BASE_ARGS:---g3-garage off}
cand_args=${G11_CAND_ARGS:---g3-garage off}
cand_src=${G11_CAND_SRC:-$repo}
max_load=${G11_MAX_LOAD:-4}
log=$work/g11.log
: > "$log"
say() { echo "$*" | tee -a "$log"; }
wait_load() {
    while [ "$(cut -d. -f1 /proc/loadavg)" -ge "$max_load" ]; do sleep 10; done
}

say "G11 replay identity: baseline $base_commit vs candidate $(git -C "$cand_src" rev-parse HEAD 2>/dev/null || echo "$cand_src")"
say "boots: $(wc -l < "$chain"); common: $common; base: $base_args; candidate: $cand_args"
# Build the baseline replay_beta from the release commit (git archive, scratch only).
if [ ! -x "$work/base-build/replay_beta" ]; then
    rm -rf "$work/base-src" && mkdir -p "$work/base-src" || exit 1
    git -C "$repo" cat-file -e "$base_commit^{commit}" 2>/dev/null || git -C "$repo" fetch -q origin "$base_commit" || {
        say "FAIL: baseline commit $base_commit not available"; exit 1; }
    git -C "$repo" archive "$base_commit" | tar -x -C "$work/base-src" || exit 1
    wait_load
    nice -n 10 make -s -C "$work/base-src" BUILD="$work/base-build" "$work/base-build/replay_beta" >> "$log" 2>&1 || {
        say 'FAIL: baseline replay_beta build'; exit 1; }
fi
wait_load
nice -n 10 make -s -C "$cand_src" BUILD="$work/cand-build" "$work/cand-build/replay_beta" >> "$log" 2>&1 || {
    say 'FAIL: candidate replay_beta build'; exit 1; }

run_chain() {  # BIN OUTDIR EXTRA_ARGS
    bin=$1; out=$2; extra=$3
    rm -rf "$out" && mkdir -p "$out/store" || exit 1
    i=0
    while read -r dir boot; do
        [ -n "$dir" ] || continue
        i=$((i + 1)); n=$(printf '%02d' "$i")_$(printf '%.8s' "$boot")
        wait_load
        # shellcheck disable=SC2086
        (cd "$out" && nice -n 19 "$bin" --trip "$dir" --boot-id "$boot" $common --g3-store store \
            --report "$n.json" --csv "$n.csv" --journal "$n.jsonl" --g3-out "$n.g3.jsonl" \
            --g3-send-out "$n.sends.jsonl" $extra > "$n.stdout" 2> "$n.stderr")
        echo "$n exit $?" >> "$out/exits.txt"
    done < "$chain"
}
say "replaying with the baseline ($(date +%H:%M:%S))"
run_chain "$work/base-build/replay_beta" "$work/base" "$base_args"
say "replaying with the candidate ($(date +%H:%M:%S))"
run_chain "$work/cand-build/replay_beta" "$work/cand" "$cand_args"
python3 - "$work/base" "$work/cand" <<'EOF' | tee -a "$log"
import os, re, sys
a, b = sys.argv[1], sys.argv[2]
norm = lambda s: re.sub(r'"(lat_ms|start_ns|end_ns)":[0-9.]+', r'"\1":X', s)
names = sorted(set(os.listdir(a)) | set(os.listdir(b)))
names = [n for n in names if n not in ('store', 'exits.txt')]
same, diff = 0, []
for n in names:
    pa, pb = os.path.join(a, n), os.path.join(b, n)
    if not (os.path.isfile(pa) and os.path.isfile(pb)):
        diff.append(n + ' (missing on one side)')
        continue
    x, y = open(pa, errors='replace').read(), open(pb, errors='replace').read()
    x, y = norm(x), norm(y)
    if x == y:
        same += 1
    else:
        diff.append(n)
for n in diff:
    print('DIFF', n)
print(f'G11: {same}/{len(names)} files identical')
sys.exit(0 if not diff and names else 1)
EOF
rc=$?
cmp -s "$work/base/exits.txt" "$work/cand/exits.txt" || { say 'exit codes differ:'; diff "$work/base/exits.txt" "$work/cand/exits.txt" | tee -a "$log"; rc=1; }
say "G11 exit $rc (work: $work)"
exit $rc
