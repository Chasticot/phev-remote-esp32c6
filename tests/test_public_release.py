"""Public package boundaries; does not inspect private config or connect externally."""
from pathlib import Path
import re
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[1]

class PublicRelease(unittest.TestCase):
    def test_dashboard_actions_exist_and_start_requires_confirmation(self):
        package = yaml.safe_load((ROOT / 'homeassistant/PHEV-HA-controls.yaml').read_text(encoding='utf-8'))
        view = yaml.safe_load((ROOT / 'homeassistant/PHEV-dashboard-view.yaml').read_text(encoding='utf-8'))
        def walk(obj):
            if isinstance(obj, dict):
                yield obj
                for value in obj.values(): yield from walk(value)
            elif isinstance(obj, list):
                for value in obj: yield from walk(value)
        actions = [o for o in walk(view) if 'perform_action' in o]
        self.assertEqual(len(actions), 7)
        for action in actions:
            script = action['perform_action'].removeprefix('script.')
            self.assertIn(script, package['script'])
            if script not in ('phev_actualiser', 'phev_arreter'):
                self.assertIn('confirmation', action)

    def test_no_identity_or_home_ssid_default(self):
        identity = (ROOT / 'include/PhevIdentity.h').read_text()
        self.assertNotRegex(identity, r'"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}"')
        service = (ROOT / 'src/MaintenanceService.cpp').read_text()
        self.assertIn('prefs_.getString("home_ssid", "")', service)

    def test_mac_saved_only_after_validation_in_ap(self):
        source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
        save = source.split('web.on("/save", HTTP_POST', 1)[1].split('web.on("/pair"', 1)[0]
        self.assertLess(save.index('!portalActive || maintenance.homeMode()'), save.index('web.arg("mac")'))
        self.assertLess(save.index('!parseMac(mac, parsed)'), save.index('prefs.putString("mac", mac)'))

    def test_pinned_platform_and_license_texts(self):
        config = (ROOT / 'platformio.ini').read_text()
        self.assertIn('/download/55.03.312/', config)
        self.assertNotIn('/download/stable/', config)
        self.assertIn('Version 3, 29 June 2007', (ROOT / 'LICENSE').read_text())
        self.assertIn('Version 2.1, February 1999', (ROOT / 'LICENSES/LGPL-2.1.txt').read_text())

    def test_source_and_examples_have_no_personal_paths(self):
        for folder in ('src', 'include', 'tests', 'tools', 'homeassistant', 'zigbee2mqtt'):
            for path in (ROOT / folder).rglob('*'):
                if path.suffix not in ('.h', '.cpp', '.py', '.ps1', '.mjs', '.yaml') or '.build' in path.parts:
                    continue
                text = path.read_text(encoding='utf-8')
                self.assertNotRegex(text, r'[A-Za-z]:[/\\]Users[/\\][^/\\]+', str(path))

if __name__ == '__main__':
    unittest.main()
