#!/bin/sh
# Full offline ND replay: windows -> production Pipeline (host build) -> summary.
set -e
W=$(cd "$(dirname "$0")" && pwd)
CSV=${1:?usage: run.sh /path/to/raw_can.csv}
"$W/build.sh"
python3 "$W/prep.py" "$CSV" "$W/windows.csv"
python3 "$W/analyze.py" "$W" | tee "$W/report.txt"
