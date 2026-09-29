"""Authored installation fixtures, explicitly NOT OEM firmware identity tests.

Run every installer scenario even when proprietary firmware is unavailable.
Only this temporary bundle's manifest is replaced with synthetic identities.
The production firmware.sha256 remains unchanged and private-fixture tests
retain their separate PASS/SKIP reporting.
"""
import hashlib
import os
from pathlib import Path
import shutil
import tempfile
from unittest.mock import patch

import test_packaging

CONFIG = '''<sm_config><services>
<service type="jci_service" name="jciAAPA" path="/jci/aapa/blmjciaapa.so" args="new_hw">
</service>
<service type="jci_service" name="jciVBS" path="/jci/vbs/svcjcivbs.so" args="">
</service>
</services></sm_config>
'''
AUTOSTART = '''#!/bin/sh
SMCFG_NORMALMODE='/jci/sm/sm.conf'
SMCFG_WCPMODE='/jci/sm/sm_WCP.conf'
case "$BOARD" in
2)
 taskset 0x02 /jci/sm/sm -f $SMCFG_WCPMODE -e /tmp/smevents.txt &
;;
*)
 taskset 0x02 /jci/sm/sm -f $SMCFG_NORMALMODE -e /tmp/smevents.txt &
;;
esac
'''


def make_stock(root):
    identities = []
    for line in (test_packaging.PACK / 'firmware.sha256').read_text().splitlines():
        name = line.split()[1]
        file = root / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(('SYNTHETIC, NOT OEM: ' + name).encode())
        identities.append(hashlib.sha256(file.read_bytes()).hexdigest() + '  ' + name)
    (root / 'jci/version.ini').write_text(
        'JCI_SW_VER="MAZ_CMU-150_74.00.324"\nJCI_SW_FLAVOR="cmu150_NA"\nJCI_SW_VER_PATCH="A"\n')
    (root / 'jci/sm').mkdir()
    for name in ('sm.conf', 'sm_WCP.conf'):
        (root / 'jci/sm' / name).write_text(CONFIG)
    (root / 'usr/bin/autostart').write_text(AUTOSTART)
    return '\n'.join(identities) + '\n'


class SyntheticPackagingTests(test_packaging.PackagingTests):
    def setUp(self):
        stock = tempfile.TemporaryDirectory()
        self.addCleanup(stock.cleanup)
        self.stock_root = Path(stock.name)
        self.synthetic_manifest = make_stock(self.stock_root)
        super().setUp()

    def test_stock_persist_alias_install_rearm_export_uninstall(self):
        (self.root / 'mnt').mkdir()
        (self.root / 'data_persist').rename(self.root / 'mnt/data_persist')
        (self.root / 'data_persist').symlink_to('/mnt/data_persist')
        self.add_touch()
        before = self.sm.read_bytes()
        self.run_script('install.sh', '--mode=SHADOW')
        base = self.root / 'mnt/data_persist/mx5-aa-dr'
        self.assertTrue((base / 'guard/normal.trial').exists())
        self.run_script('arm.sh', '--mode=SHADOW')
        dest = Path(self.tmp.name) / 'usb'
        dest.mkdir()
        self.run_script('export_logs.sh', str(dest))
        self.run_script('uninstall.sh')
        self.assertEqual(self.sm.read_bytes(), before)
        self.assertFalse((self.root / 'mnt/data_persist/.mx5dr-install-lock').exists())

    def test_full_stock_persist_chain(self):
        (self.root / 'tmp/mnt').mkdir(parents=True)
        (self.root / 'data_persist').rename(self.root / 'tmp/mnt/data_persist')
        (self.root / 'data_persist').symlink_to('/mnt/data_persist')
        (self.root / 'mnt').symlink_to('/tmp/mnt')
        self.run_script('install.sh', '--mode=SHADOW')
        self.run_script('uninstall.sh')
        self.assertEqual(self.sm.read_bytes(), self.original)

    def run_with_concurrent_touch_edit(self, script):
        commands = Path(self.tmp.name) / 'commands'
        commands.mkdir()
        awk = commands / 'awk'
        awk.write_text('''#!/bin/sh
"$REAL_AWK" "$@"
status=$?
case "$*" in
 *edit_service.awk*)
  if [ ! -e "$EDIT_MARKER" ]; then
   printf '<!-- concurrent touch change -->\\n' >> "$EDIT_TARGET"
   touch "$EDIT_MARKER"
  fi;;
esac
exit "$status"
''')
        awk.chmod(0o755)
        env = dict(PATH=str(commands) + os.pathsep + os.environ['PATH'],
                   REAL_AWK=shutil.which('awk'), EDIT_TARGET=str(self.sm),
                   EDIT_MARKER=str(Path(self.tmp.name) / 'mutated'))
        with patch.dict(os.environ, env):
            return self.run_script(script, '--mode=SHADOW', ok=False)

    def test_concurrent_edit_while_building_trial_does_not_publish_autostart(self):
        result = self.run_with_concurrent_touch_edit('install.sh')
        self.assertIn('Concurrent edit', result.stderr)
        self.assertEqual(self.autostart.read_bytes(), self.original_autostart)
        self.assertFalse(list(self.autostart.parent.glob('autostart.mx5dr-new.*')))

    def test_concurrent_touch_edit_during_rearm_leaves_no_arm(self):
        self.run_script('install.sh', '--mode=SHADOW')
        guard = self.root / 'data_persist/mx5-aa-dr/guard'
        before = (guard / 'normal.trial').read_bytes()
        (guard / 'arm').write_text('previous arm')
        result = self.run_with_concurrent_touch_edit('arm.sh')
        self.assertIn('Concurrent edit', result.stderr)
        self.assertFalse((guard / 'arm').exists())
        self.assertEqual((guard / 'normal.trial').read_bytes(), before)
        self.assertFalse(list(guard.glob('*.new.*')))

    def test_uninstall_syncs_staging_and_keeps_unchanged_sm_inodes(self):
        self.run_script('install.sh', '--mode=SHADOW')
        configs = [self.sm, self.sm.with_name('sm_WCP.conf')]
        original = {path: (path.stat().st_ino, path.read_bytes()) for path in configs}
        commands = Path(self.tmp.name) / 'commands'
        commands.mkdir()
        log = Path(self.tmp.name) / 'order.log'
        for name in ('sync', 'mv'):
            wrapper = commands / name
            wrapper.write_text('#!/bin/sh\nprintf "' + name + ' %s\\n" "$*" >> "$ORDER_LOG"\nexec "$REAL_' + name.upper() + '" "$@"\n')
            wrapper.chmod(0o755)
        with patch.dict(os.environ, dict(PATH=str(commands) + os.pathsep + os.environ['PATH'],
                ORDER_LOG=str(log), REAL_SYNC=shutil.which('sync'), REAL_MV=shutil.which('mv'))):
            self.run_script('uninstall.sh')
        for path, (inode, content) in original.items():
            self.assertEqual((path.stat().st_ino, path.read_bytes()), (inode, content))
        events = log.read_text().splitlines()
        config_move = next(i for i, e in enumerate(events) if e.startswith('mv ') and e.endswith('/mx5dr.conf'))
        oem_move = next(i for i, e in enumerate(events) if e.startswith('mv ') and e.endswith('/usr/bin/autostart'))
        self.assertTrue(any(e.startswith('sync ') for e in events[config_move + 1:oem_move]), events)
