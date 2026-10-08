from pathlib import Path
import runpy
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Environment:
    def subst(self, value):
        assert value == '$PROJECT_DIR'
        return str(ROOT)
    def PioPlatform(self): return self
    def get_package_dir(self, name):
        assert name == 'framework-arduinoespressif32'
        return str(ROOT / 'fake-packages/framework-arduinoespressif32')
    def Append(self, **values): self.flags = values['CCFLAGS']

class PrivacyBuild(unittest.TestCase):
    def test_maps_project_and_framework_package_roots(self):
        env = Environment()
        runpy.run_path(str(ROOT / 'tools/privacy_build.py'), init_globals={'env': env, 'Import': lambda _: None})
        self.assertTrue(all(f.startswith('-ffile-prefix-map=') for f in env.flags))
        self.assertTrue(any(f.endswith('=.') for f in env.flags))
        self.assertTrue(any(f.endswith('=packages') for f in env.flags))
        self.assertGreaterEqual(len(env.flags), 2)

if __name__ == '__main__': unittest.main()
