#!/bin/sh
# Host tests for the phone sender (no Gradle, no network, no Android device):
# golden vectors (the canonical tests/phone_imu/golden_frames.json, regenerated and compared), JVM core tests, and the Java-encoder vs
# independent-Python-decoder end-to-end check.
set -eu
imu_project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# The single canonical golden file of both sides (CMU parser tests read it too).
imu_repo=$(CDPATH= cd -- "$imu_project/../.." && pwd)
imu_golden=$imu_repo/tests/phone_imu/golden_frames.json
imu_tmp=$(mktemp -d)
trap 'rm -rf "$imu_tmp"' EXIT HUP INT TERM
python3 "$imu_repo/tests/phone_imu/make_golden.py" --check
python3 "$imu_project/tests/e2e_check.py" golden "$imu_golden"
javac --release 11 -Xlint:all -Werror -d "$imu_tmp/classes" \
    "$imu_project"/src/me/sixtyfive/mx5imu/core/*.java "$imu_project"/tests/java/me/sixtyfive/mx5imu/core/*.java
java -cp "$imu_tmp/classes" me.sixtyfive.mx5imu.core.CoreTests "$imu_golden" "$imu_tmp"
python3 "$imu_project/tests/e2e_check.py" pings "$imu_tmp/pings.bin" "$imu_tmp/pings.json"
java -cp "$imu_tmp/classes" me.sixtyfive.mx5imu.core.E2EDump "$imu_tmp/pings.bin" "$imu_tmp/phone.bin" "$imu_tmp/samples.csv"
python3 "$imu_project/tests/e2e_check.py" check "$imu_tmp/phone.bin" "$imu_tmp/pings.json" "$imu_tmp/samples.csv"
echo "phone imu-sender host tests passed"
