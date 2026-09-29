#!/usr/bin/env python3
"""Authored configuration fixtures for the isolated location/SM diagnostic."""
from pathlib import Path
import re
import subprocess
import unittest
import xml.etree.ElementTree as ET


HERE = Path(__file__).resolve().parent
SCRIPT = HERE / 'oem_guest_init.sh'
SELECTED = (
    'settings', 'jciUSBMGR', 'jciVBS', 'jciLDS', 'jcinavi',
    'jciBLMSettings', 'jciTime', 'aap_service', 'jciAAPA', 'stage_1',
    'stage_2', 'stage_3', 'stage_navi', 'usb_drivers', 'vim_app',
    'dbus_service', 'dbus_hmi', 'NNG', 'jciBLMTIME',
)
DEPENDENCIES = {
    'stage_2': ('jciBLMSettings',),
    'stage_3': ('jciMMUI',),
    'stage_navi': ('stage_3',),
    'settings': ('stage_1',),
    'jciBLMSettings': ('settings', 'devices', 'audio_config', 'dsp_config',
                       'system_mazda_my14'),
    'jciVBS': ('settings',),
    'jciTime': ('stage_2',),
    'jciBLMTIME': ('jciTime', 'jciBLMSettings'),
    'usb_drivers': ('settings',),
    'jciUSBMGR': ('usb_drivers',),
    'aap_service': ('devicemanager',),
    'jciAAPA': ('aap_service', 'jciBLMSettings', 'jciVBS', 'devicemanager',
                 'audio_manager', 'jciRM', 'jciUpdatea'),
    'jciLDS': ('jciUSBMGR',),
    'jcinavi': ('stage_navi', 'jciVBS', 'jciBLMSettings', 'jciLDS',
                'jciTime', 'jciUSBMGR'),
    'NNG': ('jcinavi',),
}
CONNECTIONS = {
    'jciTime': ('jciBLMTIME',),
    'jcinavi': ('NNG',),
    'jciBLMSettings': ('jciaudiosettings',),
}


def fixture():
    # These sentinel attributes/paths are authored test data, not a copy of an
    # OEM configuration. The fixed graph exercises the diagnostic selection.
    lines = [
        '<?xml version="1.0"?>', '<sm_config>',
        '  <connect service_launcher_path="/fixture/launcher"/>',
        '  <sm_server watchdog_enable="true" listen_port="19091"/>',
        '  <services retry_count="3" user_account="fixture-user">',
        '    <service name="unselected" path="/fixture/unselected"/>',
    ]
    for index, name in enumerate(SELECTED):
        lines.append(
            f'    <service name="{name}" path="/fixture/{name}" '
            f'retry_count="{index}" reset_board="yes" '
            f'autorun="{"no" if name == "NNG" else "yes"}" '
            'args="--first=one --second=two">')
        lines.append(
            f'      <environ_var env_name="KEEP_{index}" '
            'env_value="original:/fixture/preload.so"/>')
        if name == 'jciVBS':
            lines.append('      <!-- <dependency type="service" value="vim_app"/> -->')
        for tag, edges in (('dependency', DEPENDENCIES), ('connection', CONNECTIONS)):
            for target in edges.get(name, ()):
                lines.append(f'      <{tag} type="service" value="{target}"/>')
        lines.append('    </service>')
    lines.extend(('  </services>', '</sm_config>', ''))
    return '\n'.join(lines)


class LocationSmDiagnostic(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        block = SCRIPT.read_text().split('# LOCATION_SM_CONFIG_AWK_BEGIN\n', 1)[1].split(
            '# LOCATION_SM_CONFIG_AWK_END', 1)[0]
        cls.program = re.search(r"awk '(.*?)' \"\$trial\"", block, re.S).group(1)

    def reduce(self, source):
        return subprocess.run(['awk', self.program], input=source, text=True,
                              capture_output=True)

    def test_preserves_attributes_environment_and_selected_edges(self):
        source = fixture()
        result = self.reduce(source)
        self.assertEqual(result.returncode, 0, result.stderr)
        original = ET.fromstring(source)
        reduced = ET.fromstring(result.stdout)
        for tag in ('connect', 'sm_server', 'services'):
            self.assertEqual(reduced.find(tag).attrib, original.find(tag).attrib)
        services = reduced.findall('./services/service')
        self.assertEqual([service.attrib['name'] for service in services], list(SELECTED))
        for service in services:
            expected = original.find('./services/service[@name="' + service.attrib['name'] + '"]')
            self.assertEqual(service.attrib, expected.attrib)
            self.assertEqual([node.attrib for node in service.findall('environ_var')],
                             [node.attrib for node in expected.findall('environ_var')])
            for tag in ('dependency', 'connection'):
                self.assertEqual([node.attrib for node in service.findall(tag)],
                                 [node.attrib for node in expected.findall(tag)
                                  if node.attrib['value'] in SELECTED])
        self.assertEqual(len(reduced.findall('./services/service/dependency')), 21)
        self.assertEqual(len(reduced.findall('./services/service/connection')), 2)
        self.assertIn('<!-- <dependency type="service" value="vim_app"/> -->', result.stdout)

    def test_rejects_missing_or_duplicate_selected_service(self):
        for name in SELECTED:
            for source in (fixture().replace(f'name="{name}"', 'name="missing"'),
                           fixture().replace('name="unselected"', f'name="{name}"')):
                with self.subTest(name=name, source=source):
                    self.assertNotEqual(self.reduce(source).returncode, 0)

    def test_rejects_changed_edge_counts(self):
        for before, after in (
            ('value="stage_1"', 'value="missing-stage"'),
            ('value="jciMMUI"', 'value="stage_1"'),
            ('<connection type="service" value="NNG"/>', ''),
            ('<connection type="service" value="jciaudiosettings"/>', ''),
        ):
            with self.subTest(before=before):
                self.assertNotEqual(self.reduce(fixture().replace(before, after)).returncode, 0)

    def test_rejects_unclosed_selected_service(self):
        for name in SELECTED:
            source = fixture()
            block = re.search(r'    <service name="' + re.escape(name) +
                              r'".*?</service>', source, re.S).group(0)
            source = source.replace(block, block.removesuffix('</service>'))
            with self.subTest(name=name):
                self.assertNotEqual(self.reduce(source).returncode, 0)

    def test_commented_service_cannot_become_active(self):
        source = fixture().replace('args="--first=one --second=two"',
                                   'args="first=one second=two"')
        block = re.search(r'    <service name="settings".*?</service>', source, re.S).group(0)
        source = source.replace(block, '    <!--\n' + block + '\n    -->')
        self.assertIsNone(ET.fromstring(source).find('./services/service[@name="settings"]'))
        self.assertNotEqual(self.reduce(source).returncode, 0)

    def test_truncated_wrappers_are_not_repaired(self):
        source = fixture().replace('  </services>\n</sm_config>\n', '')
        self.assertNotEqual(self.reduce(source).returncode, 0)

    def test_requires_one_root_opening_before_services(self):
        original = fixture()
        for source in (
                original.replace('<sm_config>\n', ''),
                original.replace('<sm_config>', '<different_root>'),
                original.replace('<sm_config>', '<!-- <sm_config> -->'),
                original.replace('<sm_config>', '<sm_config>\n<sm_config>'),
                original.replace('<sm_config>\n', '').replace(
                    '  <services retry_count="3" user_account="fixture-user">',
                    '  <services retry_count="3" user_account="fixture-user">\n<sm_config>')):
            with self.subTest(source=source):
                self.assertNotEqual(self.reduce(source).returncode, 0)

    def test_rejects_mixed_multiline_comment_and_markup(self):
        edge = '<dependency type="service" value="stage_1"/>'
        for replacement in (edge + ' <!--\n      comment\n      -->',
                            '<!--\n      comment\n      --> ' + edge):
            source = fixture().replace(edge, replacement)
            ET.fromstring(source)  # Valid XML, unsupported diagnostic layout.
            with self.subTest(replacement=replacement):
                self.assertNotEqual(self.reduce(source).returncode, 0)


if __name__ == '__main__':
    unittest.main()
