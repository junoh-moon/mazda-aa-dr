#!/usr/bin/env python3
"""G5/G6 ZIP-level checker for a release installation ZIP.

  python3 tests/gates/g5g6_zipcheck.py --zip mazda-aa-dr-TAG.zip [--commit SHA40] \
      [--sha256 HEX] [--mode BETA] [--readelf PATH]

G6 (ZIP): `unzip -t` clean; no top-level folder (trial, install.sh at the top);
SHA256SUMS verifies and lists every other file; self-contained entry files
(mp3/ with exactly 4 .mp3, js/run.js, USB_ENTRY_NOTICE.md); build-info.json:
default_mode == --mode, install_policy == persistent, source_modified false,
source_commit 40 hex (== --commit when given), artifacts hashes == the files;
bundle-default-mode == --mode; the outer .sha256 sidecar (next to the ZIP or
--sha256) matches; INSTALL_KO.md present.
G5 (ELF) for libmx5dr.so, libmx5dr-vimtap.so, libmx5dr-ldstap.so,
mx5dr-collector, mx5dr-guard, mx5dr-sha256: ELF32 ARM EABI5, softfp (no
hard-float flag, no 'Tag_ABI_VFP_args: VFP registers'), no TEXTREL, the only
symbol version needed is GLIBC_2.4, no dynamic libstdc++/libgcc_s, the
.sha256 sidecar matches; mx5dr-sha256 is static (no dynamic section).
Exit 0 pass, 1 fail, 77 skip (no readelf)."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile

BINARIES = ('libmx5dr.so', 'libmx5dr-vimtap.so', 'libmx5dr-ldstap.so', 'mx5dr-collector', 'mx5dr-guard',
            'mx5dr-sha256')
results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok)))
    print(f'CHECK {"PASS" if ok else "FAIL"}: {name}' + (f' -- {detail}' if detail else ''), flush=True)
    return ok


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def readelf(tool, flags, path):
    return subprocess.run([tool] + flags + [str(path)], capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--zip', required=True)
    ap.add_argument('--commit')
    ap.add_argument('--sha256')
    ap.add_argument('--mode', default='BETA')
    ap.add_argument('--tunnel-md5', default=None,
                    help='Require this md5 for tunnels-v1.bin (release pin check)')
    ap.add_argument('--readelf', default=None)
    a = ap.parse_args()
    cross = os.environ.get('CROSS_COMPILE')
    tool = a.readelf or (cross + 'readelf' if cross and shutil.which(cross + 'readelf') else shutil.which('readelf'))
    if not tool:
        print('SKIP: readelf not found (set CROSS_COMPILE or --readelf)')
        return 77
    z = Path(a.zip)
    print(f'zip: {z.name} sha256 {sha(z)}')
    t = subprocess.run(['unzip', '-tq', str(z)], capture_output=True, text=True)
    check('unzip -t clean', t.returncode == 0, t.stdout.strip()[-120:])
    names = zipfile.ZipFile(z).namelist()
    check('no top-level folder: trial and install.sh at the top', 'trial' in names and 'install.sh' in names)
    side = Path(str(z) + '.sha256')
    expected = a.sha256 or (side.read_text().split()[0] if side.exists() else None)
    if expected:
        check('outer sha256 matches', sha(z) == expected, expected)
    else:
        check('outer sha256 available (sidecar or --sha256)', False)
    with tempfile.TemporaryDirectory(prefix='mx5dr-zipcheck-') as tmp:
        u = Path(tmp)
        subprocess.run(['unzip', '-q', str(z), '-d', str(u)], check=True)
        sums = subprocess.run(['sha256sum', '-c', 'SHA256SUMS'], cwd=u, capture_output=True, text=True)
        bad = [x for x in sums.stdout.splitlines() if not x.endswith(': OK')]
        check('SHA256SUMS verifies', sums.returncode == 0 and not bad, '; '.join(bad[:3]))
        listed = {line.split(None, 1)[1].lstrip('*').strip() for line in (u / 'SHA256SUMS').read_text().splitlines() if line.strip()}
        files = {p.relative_to(u).as_posix() for p in u.rglob('*') if p.is_file()} - {'SHA256SUMS'}
        check('SHA256SUMS lists every file', files == listed, f'unlisted={sorted(files - listed)[:3]} missing={sorted(listed - files)[:3]}')
        mp3 = sorted(p.name for p in (u / 'mp3').glob('*.mp3')) if (u / 'mp3').is_dir() else []
        check('mp3/ has exactly 4 entry files', len(mp3) == 4, str(mp3))
        check('js/run.js present', (u / 'js/run.js').is_file())
        check('USB_ENTRY_NOTICE.md present', (u / 'USB_ENTRY_NOTICE.md').is_file())
        check('INSTALL_KO.md present', (u / 'INSTALL_KO.md').is_file())
        info = json.loads((u / 'build-info.json').read_text())
        check(f'build-info default_mode {a.mode}', info.get('default_mode') == a.mode, str(info.get('default_mode')))
        check('build-info install_policy persistent', info.get('install_policy') == 'persistent', str(info.get('install_policy')))
        check('build-info source_modified false', info.get('source_modified') is False)
        commit = info.get('source_commit', '')
        check('build-info source_commit is 40 hex', re.fullmatch(r'[0-9a-f]{40}', commit or '') is not None, commit)
        if a.commit:
            check('build-info source_commit == pinned commit', commit == a.commit, f'{commit} vs {a.commit}')
        check(f'bundle-default-mode {a.mode}', (u / 'bundle-default-mode').read_text().strip() == a.mode)
        if a.mode == 'BETA':
            db = u / 'tunnels-v1.bin'
            if check('tunnels-v1.bin present (BETA default enables the map constraint)', db.is_file()):
                sh = hashlib.sha256(db.read_bytes()).hexdigest()
                sc = u / 'tunnels-v1.bin.sha256'
                check('tunnels-v1.bin.sha256 matches', sc.is_file() and
                      (sc.read_text().split() or [''])[0] == sh)
                if a.tunnel_md5:
                    check('tunnels-v1.bin md5 pin', hashlib.md5(db.read_bytes()).hexdigest() == a.tunnel_md5,
                          a.tunnel_md5)
            check('tunnels-v1.manifest.json present', (u / 'tunnels-v1.manifest.json').is_file())
            check('MAP_DATA_NOTICE.md present', (u / 'MAP_DATA_NOTICE.md').is_file())
        arts = info.get('artifacts', {})
        for b in BINARIES:
            p = u / b
            if not check(f'{b}: present', p.is_file()):
                continue
            check(f'{b}: build-info artifact hash', arts.get(b) == sha(p))
            sc = u / (b + '.sha256')
            check(f'{b}: .sha256 sidecar', sc.is_file() and sc.read_text().split()[0] == sha(p))
            hdr = readelf(tool, ['-h'], p)
            flags = re.search(r'Flags:\s+0x([0-9a-f]+)', hdr)
            fl = int(flags.group(1), 16) if flags else -1
            check(f'{b}: ELF32 ARM EABI5', 'ELF32' in hdr and re.search(r'Machine:\s+ARM', hdr) is not None
                  and fl >= 0 and (fl >> 24) == 5, f'flags=0x{fl:x}')
            attrs = readelf(tool, ['-A'], p)
            check(f'{b}: softfp (no hard-float flag, no VFP argument passing)',
                  fl >= 0 and not (fl & 0x400) and 'Tag_ABI_VFP_args: VFP registers' not in attrs)
            dyn = readelf(tool, ['-d'], p)
            check(f'{b}: no TEXTREL', 'TEXTREL' not in dyn)
            needed = re.findall(r'Shared library: \[([^\]]+)\]', dyn)
            if b == 'mx5dr-sha256':
                check(f'{b}: static (no dynamic section)', 'There is no dynamic section' in dyn or not needed, str(needed))
                continue
            check(f'{b}: no dynamic libstdc++/libgcc_s', not any('libstdc++' in n or 'libgcc_s' in n for n in needed), str(needed))
            versions = sorted(set(re.findall(r'Name: ([A-Za-z_]+[0-9][0-9.]*)', readelf(tool, ['-V'], p))))
            versions = [v for v in versions if not v.startswith('MX5')]
            check(f'{b}: only GLIBC_2.4 symbol versions', versions == ['GLIBC_2.4'], str(versions))
    passed = sum(ok for _, ok in results)
    print(f'\nSUMMARY G5/G6: {passed}/{len(results)} checks passed')
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
