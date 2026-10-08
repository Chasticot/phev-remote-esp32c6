"""Portable host-only runner; never opens serial, MQTT, Zigbee or HA."""
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / 'tests'

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', default='g++')
    parser.add_argument('--zig', action='store_true', help='Use zig c++ frontend')
    args = parser.parse_args()
    build = TESTS / '.build'
    build.mkdir(exist_ok=True)
    cases = [
        ('protocol', ['fakes', '../src'], ['protocol_v7_test.cpp', '../src/PhevProtocol.cpp'], []),
        ('protocol_native', ['fakes', '../src'], ['protocol_v7_test.cpp', '../src/PhevProtocol.cpp'], ['-DARDUINO_ARCH_ESP32']),
        ('maintenance', ['../include'], ['maintenance_policy_test.cpp'], []),
        ('zcl', ['../include'], ['zcl_report_test.cpp'], []),
        ('demand', ['fakes', '../include', '../src'], ['demand_policy_test.cpp'], []),
        ('session', ['../include'], ['session_result_test.cpp'], []),
        ('tcp_compat', ['tcp_compat_fakes', '../include'], ['tcp_compat_test.cpp', '../src/PhevTcpCompat.cpp'], []),
        ('identity', ['../include'], ['identity_test.cpp'], []),
    ]
    for name, includes, sources, flags in cases:
        out = build / (name + ('.exe' if sys.platform == 'win32' else ''))
        command = [args.compiler] + (['c++'] if args.zig else [])
        command += ['-std=c++17', '-Wall', '-Wextra', '-pedantic', *flags]
        command += ['-I' + str((TESTS / path).resolve()) for path in includes]
        command += [str((TESTS / path).resolve()) for path in sources]
        subprocess.run(command + ['-o', str(out)], check=True)
        subprocess.run([str(out)], check=True)
    print('PASS all 8 host suites')

if __name__ == '__main__':
    main()
