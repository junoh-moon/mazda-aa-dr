#!/usr/bin/env python3
"""Authored XML fixtures for the private OEM retry diagnostic; no OEM code runs."""
from pathlib import Path
import re
import subprocess
import tempfile
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
        cls.block = block
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

    def test_commented_service_cannot_become_active(self):
        # A double hyphen is not legal inside an XML comment.
        source = FIXTURE.replace('--uri=server:// --proxy=tcpip://',
                                 'uri=server:// proxy=tcpip://')
        for name in ('settings', 'jciAAPA'):
            block = re.search(r'    <service type="jci_service" name="' + name +
                              r'".*?</service>', source, re.S).group(0)
            commented = source.replace(block, '    <!--\n' + block + '\n    -->')
            self.assertIsNone(ET.fromstring(commented).find(
                './services/service[@name="' + name + '"]'))
            with self.subTest(name=name):
                self.assertNotEqual(self.reduce(commented).returncode, 0)

    def test_truncated_wrappers_are_not_repaired(self):
        for removed in ('  </services>\n</sm_config>\n',
                        '  </services>\n', '</sm_config>\n'):
            with self.subTest(removed=removed):
                self.assertNotEqual(self.reduce(FIXTURE.replace(removed, '')).returncode, 0)

    def test_requires_one_root_opening_before_services(self):
        for source in (
                FIXTURE.replace('<sm_config>\n', ''),
                FIXTURE.replace('<sm_config>', '<different_root>'),
                FIXTURE.replace('<sm_config>', '<!-- <sm_config> -->'),
                FIXTURE.replace('<sm_config>', '<sm_config>\n<sm_config>'),
                FIXTURE.replace('<sm_config>\n', '').replace(
                    '  <services retry_count="0" user_account="cmu">',
                    '  <services retry_count="0" user_account="cmu">\n<sm_config>')):
            with self.subTest(source=source):
                self.assertNotEqual(self.reduce(source).returncode, 0)

    def test_rejects_unclosed_selected_service(self):
        for name in ('settings', 'jciAAPA'):
            block = re.search(r'    <service type="jci_service" name="' + name +
                              r'".*?</service>', FIXTURE, re.S).group(0)
            source = FIXTURE.replace(block, block.removesuffix('</service>'))
            with self.subTest(name=name):
                self.assertNotEqual(self.reduce(source).returncode, 0)

    def test_rejects_mixed_multiline_comment_and_markup(self):
        edge = '<dependency type="service" value="stage_1"/>'
        for replacement in (edge + ' <!--\n      comment\n      -->',
                            '<!--\n      comment\n      --> ' + edge):
            source = FIXTURE.replace(edge, replacement)
            ET.fromstring(source)  # Valid XML, unsupported diagnostic layout.
            with self.subTest(replacement=replacement):
                self.assertNotEqual(self.reduce(source).returncode, 0)

    def test_failure_removes_partial_configuration(self):
        # Run the actual filter and failure branch, without launching services.
        source = FIXTURE.replace('      <dependency type="service" value="seven"/>\n', '')
        shell = 'probe() {\n' + self.block + '\n}\ntrial=$1\nretry_trial=$2\nprobe\n'
        with tempfile.TemporaryDirectory() as tmp:
            input_path = Path(tmp) / 'input.xml'
            output_path = Path(tmp) / 'partial.xml'
            input_path.write_text(source)
            result = subprocess.run(['sh', '-c', shell, 'retry-filter',
                                     str(input_path), str(output_path)],
                                    text=True, capture_output=True)
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertFalse(output_path.exists())
            self.assertIn('VM_RETRY_CONFIG_FAILED', result.stdout)

    def test_guest_shell_syntax(self):
        subprocess.run(['sh', '-n', str(SCRIPT)], check=True)


if __name__ == '__main__':
    unittest.main()
