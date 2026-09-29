"""Exercise the production account selector without changing host accounts.

Only the id/NSS boundary is supplied by the fixture. The isolated stock ARM
installation/collector checks live in cmu_emulation.py --account-regressions-only.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

PACK = Path(__file__).resolve().parents[2] / 'packaging'


class CollectorAccountTests(unittest.TestCase):
    def select(self, cmu, service):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / '.mx5dr-fixture').touch()
            commands = root / 'bin'
            commands.mkdir()
            lookup = root / 'lookups'
            command = commands / 'id'
            command.write_text('''#!/bin/sh
[ "$#" = 2 ] && [ "$1" = -u ] || exit 99
printf '%s\\n' "$2" >> "$ACCOUNT_LOOKUPS"
case "$2" in
  cmu) value=$ACCOUNT_CMU_UID;;
  service) value=$ACCOUNT_SERVICE_UID;;
  *) exit 99;;
esac
[ "$value" != missing ] || exit 1
printf '%s\\n' "$value"
''')
            command.chmod(0o755)
            env = dict(os.environ, MX5DR_FIXTURE_ROOT=str(root),
                       COMMON=str(PACK / 'common.sh'),
                       PATH=str(commands) + os.pathsep + os.environ['PATH'],
                       ACCOUNT_LOOKUPS=str(lookup),
                       ACCOUNT_CMU_UID='missing' if cmu is None else str(cmu),
                       ACCOUNT_SERVICE_UID='missing' if service is None else str(service))
            result = subprocess.run(['/bin/sh', '-c', '. "$COMMON"\ncollector_user'],
                                    env=env, capture_output=True, text=True)
            return result, lookup.read_text().splitlines()

    def test_nonroot_cmu_remains_the_writer_even_when_service_exists(self):
        result, lookups = self.select(500, 1001)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'cmu\n')
        self.assertEqual(lookups, ['cmu'])

    def test_nonroot_cmu_does_not_require_service(self):
        result, lookups = self.select(500, None)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'cmu\n')
        self.assertEqual(lookups, ['cmu'])

    def test_uid_zero_cmu_selects_service(self):
        result, lookups = self.select(0, 1001)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, 'service\n')
        self.assertEqual(lookups, ['cmu', 'service'])

    def test_uid_zero_cmu_requires_an_existing_service_account(self):
        result, lookups = self.select(0, None)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')
        self.assertIn('service account unavailable', result.stderr)
        self.assertEqual(lookups, ['cmu', 'service'])

    def test_two_uid_zero_accounts_cannot_run_the_collector(self):
        result, lookups = self.select(0, 0)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')
        self.assertIn('service must have a nonzero UID', result.stderr)
        self.assertEqual(lookups, ['cmu', 'service'])

    def test_missing_cmu_is_not_hidden_by_a_service_fallback(self):
        result, lookups = self.select(None, 1001)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')
        self.assertIn('cmu account unavailable', result.stderr)
        self.assertEqual(lookups, ['cmu'])


if __name__ == '__main__':
    unittest.main()
