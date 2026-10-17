#!/usr/bin/env python3
"""Hardware-free production C suites, sanitizers and GCC line coverage; no fetch/flash."""
import argparse
import gzip
import json
import os
from pathlib import Path
import shutil
import subprocess

REPO = Path(__file__).resolve().parents[2]
FIRMWARE = REPO / 'firmware'
CORE = [FIRMWARE / 'src' / name for name in ('gateway.c', 'gateway_registry.c', 'gateway_journal.c',
    'gateway_cockpit.c', 'gateway_config.c', 'gateway_layout.c', 'gateway_publication.c')]
CORE += [FIRMWARE / 'shared-publication/src/publication.c', FIRMWARE / 'shared-cyphal/src/gateway_protocol.c',
    FIRMWARE / 'shared-cyphal/src/cyphal.c', FIRMWARE / 'shared-cyphal/vendor/libcanard/canard.c']
INCLUDES = [FIRMWARE / name for name in ('include', 'shared-publication/include', 'shared-cyphal/include',
    'shared-cyphal/generated', 'shared-cyphal/vendor/libcanard')]
SUITES = {'gateway': FIRMWARE / 'tests/gateway_test.c', 'publication': FIRMWARE / 'tests/publication_test.c',
    'journal': FIRMWARE / 'tests/journal_test.c', 'transport': FIRMWARE / 'shared-cyphal/tests/transport_test.c'}
GATED = {'gateway.c', 'gateway_registry.c', 'gateway_cockpit.c', 'gateway_config.c', 'gateway_layout.c', 'gateway_publication.c'}

def run(command, directory, **options):
    result = subprocess.run(list(map(str, command)), cwd=directory, check=False, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **options)
    if result.stdout:
        print(result.stdout, end='', flush=True)
    result.check_returncode()
    return result.stdout

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=REPO / '.research/publication-native')
    parser.add_argument('--cc', default=os.environ.get('CC', 'cc'))
    parser.add_argument('--gcov', default='gcov')
    args = parser.parse_args()
    compiler, gcov = shutil.which(args.cc), shutil.which(args.gcov)
    if not compiler or not gcov:
        parser.error('A host C11 compiler and matching GCC gcov are required')
    output = args.build_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    flags = ['-std=c11', '-g', '-Wall', '-Wextra', '-Werror', '-UNDEBUG',
        *[arg for path in INCLUDES for arg in ('-I', str(path))]]
    summary = {'hardwareFree': True, 'physicalQualification': False, 'suites': {}, 'coverage': {}}
    for mode, extra in [('sanitizer', ['-O1', '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie']),
                        ('coverage', ['-O0', '--coverage'])]:
        directory = output / mode
        directory.mkdir(exist_ok=True)
        for old in directory.glob('*.gcda'):
            old.unlink()
        objects = []
        for source in CORE:
            obj = directory / (source.stem + '.o')
            run([compiler, *flags, *extra, '-c', source, '-o', obj], directory)
            objects.append(obj)
        for name, source in SUITES.items():
            executable = directory / name
            run([compiler, *flags, *extra, source, *objects, '-o', executable], directory)
            run([executable], directory)
            summary['suites'][f'{mode}/{name}'] = 'passed'
    for name in sorted(GATED):
        gcno = output / 'coverage' / (Path(name).stem + '.gcno')
        run([gcov, '--json-format', gcno], output / 'coverage')
        report = output / 'coverage' / (Path(name).stem + '.gcov.json.gz')
        with gzip.open(report, 'rt') as stream:
            data = json.load(stream)
        lines = [line for file in data['files'] if Path(file['file']).name == name for line in file['lines']]
        covered = sum(line['count'] > 0 for line in lines)
        percent = 100 * covered / len(lines)
        summary['coverage'][name] = {'covered': covered, 'count': len(lines), 'percent': percent}
        print(f'{name}: {covered}/{len(lines)} ({percent:.2f}%)', flush=True)
        if percent <= 70:
            raise RuntimeError(f'{name} line coverage must exceed 70%')
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f'Publication validation passed: {output / "summary.json"}', flush=True)

if __name__ == '__main__':
    main()
