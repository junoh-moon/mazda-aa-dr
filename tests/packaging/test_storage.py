"""Storage handling with real scripts/files and authored df readings."""
import os
from pathlib import Path
import unittest
from unittest.mock import patch

import test_synthetic_install


class StorageTests(unittest.TestCase):
    def setUp(self):
        self.fixture = test_synthetic_install.SyntheticPackagingTests()
        self.addCleanup(self.fixture.doCleanups)
        self.fixture.setUp()
        self.commands = Path(self.fixture.tmp.name) / 'storage-commands'
        self.commands.mkdir()
        self.base = self.fixture.root / 'data_persist/mx5-aa-dr'

    def free_space(self, kib):
        df = self.commands / 'df'
        df.write_text('#!/bin/sh\n'
                      "printf 'Filesystem 1024-blocks Used Available Capacity Mounted on\\n'\n"
                      f"printf '/dev/fixture 131072 65536 {kib} 50%% /data_persist\\n'\n")
        df.chmod(0o755)
        return patch.dict(os.environ, PATH=str(self.commands) + os.pathsep + os.environ['PATH'])

    def test_low_space_install_does_not_modify_startup_or_create_backups(self):
        with self.free_space(4096):
            r = self.fixture.run_script('install.sh', ok=False)
        self.assertIn('Insufficient persistent space', r.stderr)
        self.assertEqual(self.fixture.autostart.read_bytes(), self.fixture.original_autostart)
        self.assertFalse(self.base.exists())

    def test_install_includes_remaining_log_capacity_in_its_budget(self):
        with self.free_space(32 * 1024):
            r = self.fixture.run_script('install.sh', ok=False)
        self.assertIn('required_kib=', r.stdout)
        self.assertFalse(self.base.exists())

    def test_existing_bounded_logs_are_reused_without_demanding_twice_the_space(self):
        logs = self.base / 'logs'
        logs.mkdir(parents=True)
        # Files already at the per-file caps (trace 40 MiB x3, collector 4 MiB x2,
        # phone IMU 1 MiB x2).
        for stream, count, size in (('trace', 3, 40 * 1024 * 1024),
                                    ('collector', 2, 4 * 1024 * 1024),
                                    ('pimu', 2, 1024 * 1024)):
            for i in range(count):
                with (logs / f'{stream}.{i}.jsonl').open('wb') as f:
                    f.truncate(size)
        with self.free_space(16 * 1024):
            r = self.fixture.run_script('install.sh')
        self.assertIn('remaining_log_kib=0', r.stdout)
        self.assertEqual((logs / 'trace.0.jsonl').stat().st_size, 40 * 1024 * 1024)

    def sparse(self, name, size):
        logs = self.base / 'logs'
        logs.mkdir(parents=True, exist_ok=True)
        with (logs / name).open('wb') as f:
            f.truncate(size)

    @staticmethod
    def field(output, key):
        return int(output.split(key + '=', 1)[1].split()[0])

    def test_persistent_install_budgets_its_own_caps_not_the_one_boot_caps(self):
        # v1.0 stable: the persistent BETA install writes 3 x 16 MiB trace
        # (mx5dr.conf), the collector keeps 2 x 1 MiB in that profile and the
        # phone channel 2 x 1 MiB: 52 MiB of logs, not the 130 MiB one-boot
        # bound that refused installs needlessly.
        with self.free_space(10 ** 7):
            beta = self.fixture.run_script('install.sh', '--mode=BETA')
        self.assertEqual(self.field(beta.stdout, 'remaining_log_kib'), 52 * 1024)
        need = self.field(beta.stdout, 'required_kib')
        # BETA also carries the ~1 MiB tunnel database into the persistent
        # directory; the budget must reserve it explicitly.
        self.assertEqual(self.field(beta.stdout, 'map_db_kib'), 1024)
        self.assertEqual(need, 8192 + 64 + 1024 + 1024 + 52 * 1024 + 1)
        conf = (self.base / 'mx5dr.conf').read_text()
        self.assertIn('max_log_bytes=16777216\nmax_log_files=3\n', conf)
        with self.free_space(need):
            self.fixture.run_script('install.sh', '--mode=BETA')
        with self.free_space(need - 1):
            r = self.fixture.run_script('install.sh', '--mode=BETA', ok=False)
        self.assertIn('Insufficient persistent space', r.stderr)
        # One-boot trials keep their larger caps (3 x 40 MiB, 2 x 4 MiB).
        with self.free_space(10 ** 7):
            shadow = self.fixture.run_script('install.sh', '--mode=SHADOW')
        self.assertEqual(self.field(shadow.stdout, 'remaining_log_kib'), 130 * 1024)
        with self.free_space(need):
            self.fixture.run_script('install.sh', '--mode=SHADOW', ok=False)

    def test_persistent_install_removes_rotated_traces_above_the_cap(self):
        # Rotated 40 MiB files of earlier builds are removed (and counted as
        # free space); trace.0 (the running product may append to it) and
        # files within the cap stay; one-boot installs never remove a trace.
        big, small = 40 * 1024 * 1024, 16 * 1024 * 1024
        self.sparse('trace.0.jsonl', big)
        self.sparse('trace.1.jsonl', big)
        self.sparse('trace.2.jsonl', small)
        with self.free_space(10 ** 7):
            r = self.fixture.run_script('install.sh', '--mode=SHADOW')
        self.assertNotIn('Removed old', r.stdout)
        logs = self.base / 'logs'
        self.assertEqual((logs / 'trace.1.jsonl').stat().st_size, big)
        # Free space without the reclaimed file would be too little.
        with self.free_space(4 * 1024):
            r = self.fixture.run_script('install.sh', '--mode=BETA')
        self.assertIn('reclaimed_old_trace_kib=40960', r.stdout)
        self.assertIn('Removed old trace.1.jsonl (41943040 bytes, above the 16 MiB cap of this version).', r.stdout)
        self.assertFalse((logs / 'trace.1.jsonl').exists())
        self.assertEqual((logs / 'trace.0.jsonl').stat().st_size, big)
        self.assertEqual((logs / 'trace.2.jsonl').stat().st_size, small)
        # A symlink is refused as before (regular files only), never followed.
        outside = self.fixture.root / 'outside-trace'
        with outside.open('wb') as f:
            f.truncate(big)
        (logs / 'trace.1.jsonl').symlink_to(outside)
        with self.free_space(10 ** 7):
            r = self.fixture.run_script('install.sh', '--mode=BETA', ok=False)
        self.assertIn('Not a regular non-symlink file', r.stderr)
        self.assertEqual(outside.stat().st_size, big)

    def test_backup_sets_beyond_the_three_newest_are_removed(self):
        # Sets are ordered by their tx-seq number (the CMU clock restarts at
        # 1970, so names are nominal); sets of earlier builds (no tx-seq) are
        # the oldest. The newest original pre-install record is kept too.
        # Other entries in backups/ are never touched.
        backups = self.base / 'backups'
        backups.mkdir(parents=True)
        legacy = backups / '29991231T235959-7'          # sorts last by name
        legacy.mkdir()
        (legacy / 'autostart.before').write_text('old\n')
        (backups / 'persist-evidence/1').mkdir(parents=True)
        (backups / 'bt-fail').write_text('kept\n')
        outside = self.fixture.root / 'outside-set'
        outside.mkdir()
        (outside / 'keep').write_text('x')
        (backups / '19700101T000000-1').symlink_to(outside, target_is_directory=True)
        seen = []
        with self.free_space(10 ** 7):
            for _ in range(5):
                self.fixture.run_script('install.sh', '--mode=BETA')
                sets = sorted(p.name for p in backups.iterdir()
                              if p.is_dir() and not p.is_symlink() and 'T' in p.name)
                seen.append(sorted(int((backups / n / 'tx-seq').read_text()) if (backups / n / 'tx-seq').exists() else 0
                                   for n in sets))
        # Set 1 is the only original pre-install record (later installs back
        # up an autostart that already carries the package blocks): it is
        # kept as a fourth set so the menu 6 comparison keeps its basis.
        self.assertEqual(seen, [[0, 1], [0, 1, 2], [1, 2, 3], [1, 2, 3, 4], [1, 3, 4, 5]])
        self.assertFalse(legacy.exists())
        installed = (self.base / 'installed.txt').read_text()
        current = installed.split('backup=', 1)[1].split()[0]
        self.assertEqual((Path(current) / 'tx-seq').read_text(), '5\n')
        self.assertTrue((backups / 'persist-evidence/1').is_dir())
        self.assertEqual((backups / 'bt-fail').read_text(), 'kept\n')
        self.assertTrue((backups / '19700101T000000-1').is_symlink())
        self.assertEqual((outside / 'keep').read_text(), 'x')

    def test_export_cannot_accumulate_archives_on_persistent_storage(self):
        self.fixture.run_script('install.sh')
        r = self.fixture.run_script('export_logs.sh', str(self.base), ok=False)
        self.assertIn('outside persistent storage', r.stderr)
        self.assertFalse(list(self.base.glob('mx5dr-logs-*')))
