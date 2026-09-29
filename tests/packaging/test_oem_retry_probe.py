#!/usr/bin/env python3
"""Authored XML fixtures for the private OEM retry diagnostic; no OEM code runs."""
from pathlib import Path
import re
import subprocess
import unittest
import xml.etree.ElementTree as ET


HERE = Path(__file__).resolve().parent
SCRIPT = HERE / 'oem_guest_init.sh'
FIXTURE = '''<?xml version="1.0"?>
<sm_config>
  <connect service_launcher_path="/jci/sm/sm_svclauncher"/>
  <sm_server watchdog_enable="true"/>
  <services retry_count="0" user_account="cmu">
    <service type="process" name="unrelated" path="/not-executed"/>
    <service type="jci_service" name="settings" path="/jci/settings/svc-com-jci-cpp-settings.so" retry_count="0" reset_board="yes" args="--uri=server:// --proxy=tcpip://">
      <dependency type="service" value="stage_1"/>
      <environ_var env_name="KEEP_SETTINGS" env_value="unchanged"/>
    </service>
    <service type="jci_service" name="jciAAPA" path="/jci/aapa/blmjciaapa.so" retry_count="0" reset_board="yes" ping_timeout="30000">
      <environ_var env_name="LD_PRELOAD" env_value="/trial/lib.so:/existing/touch.so"/>
      <dependency type="service" value="one"/>
      <dependency type="service" value="two"/>
      <dependency type="service" value="three"/>
      <dependency type="service" value="four"/>
      <dependency type="service" value="five"/>
      <dependency type="service" value="six"/>
      <dependency type="service" value="seven"/>
    </service>
  </services>
</sm_config>
'''


class RetryDiagnostic(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        script = SCRIPT.read_text()
        block = script.split('# RETRY_CONFIG_AWK_BEGIN\n', 1)[1].split(
            '# RETRY_CONFIG_AWK_END', 1)[0]
        cls.program = re.search(r"awk '(.*?)' \"\$trial\"", block, re.S).group(1)

    def reduce(self, source):
        return subprocess.run(['awk', self.program], input=source, text=True,
                              capture_output=True)

    def test_preserves_service_identity_policy_and_preload(self):
        result = self.reduce(FIXTURE)
        self.assertEqual(result.returncode, 0, result.stderr)
        original = ET.fromstring(FIXTURE)
        reduced = ET.fromstring(result.stdout)
        self.assertEqual(reduced.find('connect').attrib, original.find('connect').attrib)
        self.assertEqual(reduced.find('sm_server').attrib, original.find('sm_server').attrib)
        self.assertEqual(reduced.find('services').attrib, original.find('services').attrib)
        services = reduced.findall('./services/service')
        self.assertEqual([item.attrib['name'] for item in services], ['settings', 'jciAAPA'])
        for actual in services:
            expected = original.find('./services/service[@name="' + actual.attrib['name'] + '"]')
            self.assertEqual(actual.attrib, expected.attrib)
            self.assertFalse(actual.findall('dependency'))
            self.assertEqual([item.attrib for item in actual.findall('environ_var')],
                             [item.attrib for item in expected.findall('environ_var')])

    def test_missing_or_duplicate_target_is_not_a_valid_diagnostic(self):
        for source in (FIXTURE.replace('name="jciAAPA"', 'name="otherAA"'),
                       FIXTURE.replace('name="settings"', 'name="otherSettings"'),
                       FIXTURE.replace('name="unrelated"', 'name="jciAAPA"'),
                       FIXTURE.replace('name="unrelated"', 'name="settings"')):
            with self.subTest(source=source):
                self.assertNotEqual(self.reduce(source).returncode, 0)

    def test_changed_dependency_graph_is_rejected(self):
        source = FIXTURE.replace('      <dependency type="service" value="seven"/>\n', '')
        self.assertNotEqual(self.reduce(source).returncode, 0)

    def test_guest_shell_syntax(self):
        subprocess.run(['sh', '-n', str(SCRIPT)], check=True)


if __name__ == '__main__':
    unittest.main()
