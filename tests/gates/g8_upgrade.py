#!/usr/bin/env python3
"""G8 upgrade gate: the PREVIOUS public ZIP installed and running, then the
CANDIDATE ZIP over it with digits only (menu 1, menu 5, menu 2).

  python3 tests/gates/g8_upgrade.py --previous PREV.zip --previous-sha256 HEX \
      --candidate CAND.zip [--libpatch DIR] [--exp-markers skip|present|absent] \
      [--trace-cleanup skip|yes|no]

Phases (stock-BusyBox replica, see emu.py):
 1. previous ZIP: menu 1, menu 5, product boot, menu 2 GO;
 2. owner switches set with the previous ZIP's own menus 7, 8, 10, 11, 12
    (each only if that menu exists), an authored previous-boot token, worker
    boot file and old 40 MiB-era trace files (17 MiB each, plus a stray
    trace.3.jsonl);
 3. USB swapped to the candidate: menu 1, then the checks below, menu 5,
    product boot (normal and WCP branch), menu 2 GO.
Checks: install rc 0 and persistent; no pending transaction; owner switches
listed in --cleared are gone; experimental markers (--exp-markers, default
skip until the candidate installer creates them) present/absent; previous-
product-boot token removed (--token-cleared; beta.17's installer keeps it); trace cleanup (--trace-cleanup, default skip);
every stock file byte-identical (autostart: after removing the owned blocks);
probation fenced to the installing boot; product boot with all three
preloads; confirmation; GO.
Exit 0 pass, 1 fail, 77 skip (missing environment)."""
import argparse
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import emu  # noqa: E402

TOGGLES = {'7': 'vimchan-off', '8': 'g3-off', '10': 'acc-cap-off', '11': 'bt-off', '12': 'g3-garage-off'}
TRACE_CAP = 16777216  # persistent profile: 3 x 16 MiB (packaging/common.sh)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--previous', default=os.environ.get('MX5DR_GATE_PREVIOUS'))
    ap.add_argument('--previous-sha256', default=os.environ.get('MX5DR_GATE_PREVIOUS_SHA256'))
    ap.add_argument('--candidate', default=os.environ.get('MX5DR_GATE_CANDIDATE'))
    ap.add_argument('--libpatch', default=os.environ.get('MX5DR_LIBPATCH_DIR'))
    ap.add_argument('--cleared', default=os.environ.get('MX5DR_GATE_CLEARED', 'vimchan-off,g3-off,acc-cap-off'))
    ap.add_argument('--exp-markers', default=os.environ.get('MX5DR_GATE_EXP_MARKERS', 'skip'),
                    choices=('skip', 'present', 'absent'))
    ap.add_argument('--exp-names', default=os.environ.get('MX5DR_GATE_EXP_NAMES', 'g3-garage-off,bt-off'))
    ap.add_argument('--token-cleared', default=os.environ.get('MX5DR_GATE_TOKEN_CLEARED', 'yes'),
                    choices=('skip', 'yes', 'no'))
    ap.add_argument('--trace-cleanup', default=os.environ.get('MX5DR_GATE_TRACE_CLEANUP', 'skip'),
                    choices=('skip', 'yes', 'no'))
    a = ap.parse_args()
    if not a.previous or not a.candidate:
        emu.skip('--previous and --candidate (or MX5DR_GATE_PREVIOUS/MX5DR_GATE_CANDIDATE) are required')
    stock, proot, qemu, work = emu.environment()
    if a.previous_sha256 and Path(a.previous).is_file():
        emu.verify_zip_sha256(a.previous, a.previous_sha256)
    prev = emu.unpack(a.previous, work / 'g8-previous')
    cand = emu.unpack(a.candidate, work / 'g8-candidate')
    r = emu.Replica(work, 'g8', stock, proot, qemu)
    r.emit(f'G8 upgrade: previous={Path(a.previous).name} candidate={Path(a.candidate).name}')
    if a.previous_sha256:
        r.check('previous ZIP sha256 matches', True, a.previous_sha256)
    r.build(prev, a.libpatch)

    r.emit('\n#### 1. Previous ZIP: menu 1, menu 5, product boot, menu 2')
    rc, screen = r.menu('1\n0\n', 'previous: menu 1 install')
    r.check('previous install rc 0', rc == 0, f'rc={rc}')
    r.check('previous install is persistent', 'Installed BETA persistent' in screen)
    rc, screen = r.menu('5\n', 'previous: menu 5 reboot (witness)')
    r.check('previous menu 5 calls reboot', (r.root / 'reboot.witness').exists())
    chosen = r.autostart_boot('previous product boot')
    r.check('previous: product boot with all preloads', r.trial_complete(chosen), chosen)
    screen, block = r.status('previous product boot')
    r.check(f'previous: menu 2 product this boot, no NO row, GO or WAIT (runtime rows)',
            'ok   BOOT  product this boot' in block and r.install_side_go(block)[0], r.install_side_go(block)[1])

    r.emit('\n#### 2. Owner switches with the previous menus, tokens, old trace files')
    prev_menu = (prev / 'trial').read_text()
    for digit, name in TOGGLES.items():
        if f'{digit}) if ! ' not in prev_menu:
            r.emit(f'(previous ZIP has no menu {digit}; {name} not set)')
            continue
        marker = r.base / name
        if marker.exists():
            continue
        rc, screen = r.menu(f'{digit}\n{digit}\n0\n', f'previous: menu {digit} switch ({name})')
        r.check(f'previous menu {digit} wrote {name}', marker.exists(), f'rc={rc}')
    logs = r.base / 'logs'
    logs.mkdir(exist_ok=True)
    (logs / 'g3-last-start').write_text('mx5g3start 1 %s\n' % r.boot_id(r.boot_number))
    (logs / 'g3-worker-boot').write_text('mx5g3worker 1 %s\n' % r.boot_id(r.boot_number))
    for i in range(3):
        with open(logs / f'trace.{i}.jsonl', 'wb') as f:
            f.write((b'{"kind":"old_trace_fixture"}\n' * (TRACE_CAP // 28 + 4096))[:TRACE_CAP + 1048576])
    (logs / 'trace.3.jsonl').write_text('{"kind":"stray_old_trace"}\n')
    before_markers = {n: (r.base / n).exists() for n in TOGGLES.values()}
    r.emit('markers before the upgrade: ' + str(before_markers))

    r.emit('\n#### 3. Candidate ZIP over the running previous install: menu 1')
    r.reboot('owner parks again later (new boot)')
    r.swap_usb(cand)
    rc, screen = r.menu('1\n0\n', 'candidate: menu 1 install over the previous install')
    r.check('candidate install rc 0', rc == 0, f'rc={rc}')
    r.check('candidate install is persistent', 'Installed BETA persistent' in screen)
    r.check('no pending transaction left', not (r.base / 'pending').exists())
    r.check('installed bundle is the candidate',
            (r.base / 'libmx5dr.so').read_bytes() == (cand / 'libmx5dr.so').read_bytes())
    for name in [x for x in a.cleared.split(',') if x]:
        if before_markers.get(name):
            r.check(f'owner switch {name} cleared by menu 1', not (r.base / name).exists())
        else:
            r.skipped(f'owner switch {name} cleared by menu 1', 'the previous ZIP could not set it')
    for name in [x for x in a.exp_names.split(',') if x]:
        label = f'experimental marker {name} {a.exp_markers} after the candidate install'
        if a.exp_markers == 'skip':
            r.skipped(label, f'--exp-markers skip (now: {"present" if (r.base / name).exists() else "absent"})')
        else:
            r.check(label, (r.base / name).exists() == (a.exp_markers == 'present'))
    if a.token_cleared == 'skip':
        r.skipped('previous-product-boot token removed', '--token-cleared skip')
    else:
        r.check('previous-product-boot token ' + ('removed' if a.token_cleared == 'yes' else 'kept'),
                (logs / 'g3-last-start').exists() == (a.token_cleared == 'no'))
    sizes = {p.name: p.stat().st_size for p in sorted(logs.glob('trace.*.jsonl'))}
    r.emit('trace files after the candidate install: ' + str(sizes))
    if a.trace_cleanup == 'skip':
        r.skipped('old trace files cleaned', f'--trace-cleanup skip (sizes {sizes})')
    else:
        clean = all(s <= TRACE_CAP for s in sizes.values()) and not any(
            n not in ('trace.0.jsonl', 'trace.1.jsonl', 'trace.2.jsonl') for n in sizes)
        r.check('old trace files ' + ('cleaned' if a.trace_cleanup == 'yes' else 'kept'),
                clean == (a.trace_cleanup == 'yes'), str(sizes))
    r.oem_identical('after the candidate install')
    st = r.persist_state()
    r.check('probation fenced to the installing boot', st.get('enabled_boot') == r.boot_id(r.boot_number)
            and st.get('probation') == 'yes', str(st))

    r.emit('\n#### 4. Candidate: menu 5, product boots, menu 2')
    rc, screen = r.menu('5\n', 'candidate: menu 5 reboot (witness)')
    chosen = r.autostart_boot('candidate first product boot')
    r.check('candidate: product boot with all preloads', r.trial_complete(chosen), chosen)
    r.check('candidate: confirmed by the real ARM guard', r.persist_state().get('confirmed_boot') ==
            r.boot_id(r.boot_number), str(r.persist_state()))
    screen, block = r.status('candidate product boot')
    r.check('candidate: menu 2 product this boot', 'ok   BOOT  product this boot' in block)
    r.check('candidate: menu 2 no NO row, GO or WAIT (runtime rows)', r.install_side_go(block)[0],
            r.install_side_go(block)[1])
    chosen = r.autostart_boot('candidate WCP board', var='SMCFG_WCPMODE')
    r.check('candidate: WCP branch product boot', r.trial_complete(chosen), chosen)
    r.oem_identical('after the candidate product boots')
    return r.summary('G8')


if __name__ == '__main__':
    sys.exit(main())
