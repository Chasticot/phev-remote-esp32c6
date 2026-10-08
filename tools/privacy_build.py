"""Remove local absolute project/SDK paths from compiler-emitted file strings."""
Import('env')
from pathlib import Path

project = Path(env.subst('$PROJECT_DIR'))
framework = Path(env.PioPlatform().get_package_dir('framework-arduinoespressif32'))
flags = []
for path, replacement in ((project, '.'), (framework.parent, 'packages')):
    for spelling in {str(path), path.as_posix()}:
        flags.append(f'-ffile-prefix-map={spelling}={replacement}')
env.Append(CCFLAGS=flags)
