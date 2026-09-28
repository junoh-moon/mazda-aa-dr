#!/usr/bin/env python3
"""Black-box parser and state-output tests for the offline replay executable."""
import csv
import io
import pathlib
import subprocess
import sys
import tempfile

exe = sys.argv[1]
fixture = pathlib.Path(__file__).with_name('synthetic_straight.csv')
def invoke(path):
    return subprocess.run([exe, str(path)], text=True, capture_output=True)
a = invoke(fixture)
b = invoke(fixture)
assert a.returncode == b.returncode == 0
assert a.stdout == b.stdout, 'same input/config must be byte-identical'
rows = list(csv.DictReader(io.StringIO(a.stdout)))
shots = [r for r in rows if r['event'] == 'SNAPSHOT']
assert [r['result'] for r in shots] == ['OK', 'E_STALE', 'E_NO_SEED']
assert float(shots[0]['distance_m']) == 20.0
assert shots[0]['frontier_ns'] == '2000000000'
assert shots[0]['derived_utc_ns'] == '1700000001000000000'
assert all(r['valid'] == '0' for r in shots[1:])
lines = fixture.read_text().splitlines()
with tempfile.TemporaryDirectory(prefix='mx5-replay-test-') as tmp:
    path = pathlib.Path(tmp) / 'input.csv'
    for text in ['RESET,1,1\n', 'RESET,-1,1,1\n', 'RESET,1,1,18446744073709551616\n',
                 'RESET,1,1,1\nSEED,1\n', 'SNAPSHOT,1,1,1,1\n', 'RESET,1,,1\n']:
        path.write_text(text)
        result = invoke(path)
        assert result.returncode == 2 and 'malformed' in result.stderr
    changed = list(lines)
    row = changed[4].split(',')
    row[11] = '4094'  # preserve otherwise valid claimed normalized input
    changed[4] = ','.join(row)
    path.write_text('\n'.join(changed)+'\n')
    result = invoke(path)
    assert result.returncode == 0
    data = list(csv.DictReader(io.StringIO(result.stdout)))
    assert next(r for r in data if r['event'] == 'MOTION')['result'] == 'E_QUALITY'
    assert all(r['valid'] == '0' for r in data)
print('replay tests: deterministic output, stale/reacquire, sentinel bypass and six malformed inputs passed')
