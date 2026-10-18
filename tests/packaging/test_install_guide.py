"""The one-page install guide shipped in the ZIP (packaging/USB_START_KO.md ->
INSTALL_KO.md) matches the menu it describes (v1.0 stable).

The owner's keyboard has no Shift: every command the guide asks to type is
lowercase letters, digits, spaces and slashes; every key sequence is digits.
Screen texts the guide quotes must exist in the shipped scripts.
"""
from pathlib import Path
import re
import unittest

PACK = Path(__file__).resolve().parents[2] / 'packaging'
GUIDE = (PACK / 'USB_START_KO.md').read_text()
SCRIPTS = ''.join((PACK / n).read_text() for n in ('trial', 'install.sh', 'uninstall.sh', 'common.sh'))


class InstallGuideTests(unittest.TestCase):
    def test_typed_commands_are_shift_free(self):
        blocks = re.findall(r'```sh\n(.*?)```', GUIDE, re.S)
        self.assertEqual(blocks, ['sh /tmp/mnt/sda1/trial\n'])
        for line in blocks[0].splitlines():
            self.assertRegex(line, r'^[a-z0-9 /]+$')
        # Inline key sequences are digits only.
        for span in re.findall(r'`([^`]+)`', GUIDE):
            if re.fullmatch(r'[0-9 ]+', span):
                continue
            self.assertNotRegex(span, r'^[0-9]+[A-Za-z_]')

    def test_no_stale_or_outside_references(self):
        for stale in ('SHADOW', '128', 'http', 'docs/', 'github', 'validation/', '9 Tunnel', '1-11'):
            self.assertNotIn(stale, GUIDE)

    def test_steps_name_the_exact_screen_texts(self):
        for quoted in ('Installation finished', 'GO', 'ok   CONF  confirmed', 'WAIT 60 s, then run 2 again',
                       'stock this boot', 'NO-GO: tripped', 'Install lock present', 'Incomplete transaction',
                       'EXP none'):
            self.assertIn('`%s`' % quoted, GUIDE)
            self.assertIn(quoted.replace('ok   CONF  confirmed', '"CONF","confirmed"'), SCRIPTS)

    def test_menu_table_matches_the_menu(self):
        rows = dict(re.findall(r'^\| ([0-9, ]+) \| (.*) \|$', GUIDE, re.M))
        numbers = set()
        for key in rows:
            numbers |= {n.strip() for n in key.split(',')}
        self.assertEqual(numbers, {'0', '1', '2', '3', '4', '5', '6', '7', '8', '10', '11', '12'})
        self.assertIn('9번은 없습니다', GUIDE)
        self.assertIn('꺼짐', rows['11'])
        self.assertIn('꺼짐', rows['12'])
        self.assertIn('`5`', rows['3'])
        for n in ('0', '1', '2', '3', '4', '5', '6', '7', '8', '10', '11', '12'):
            self.assertRegex(SCRIPTS, r"\n\s+%s\) " % n)
        self.assertNotRegex(SCRIPTS, r"\n\s+9\) ")

    def test_removal_and_recovery_sequences(self):
        self.assertIn('`4` → 메뉴를 다시 열어 `5` → 다시 열어 `2`', GUIDE)
        self.assertIn('`4` → 메뉴를 다시 열어 `1` → `5` → `2`', GUIDE)
        self.assertIn('`6`을 두 번', GUIDE)
        self.assertIn('USB_ENTRY_NOTICE.md', GUIDE)


if __name__ == '__main__':
    unittest.main()
