#!/usr/bin/env python3
"""Run native tests and require more than 70% line coverage of gateway action routing."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess

REPO = Path(__file__).resolve().parents[2]


def tool(name):
    resolved = shutil.which(name)
    if resolved:
        return resolved
    if shutil.which('xcrun'):
        result = subprocess.run(['xcrun', '--find', name], capture_output=True, text=True)
        if result.returncode == 0:
            return result.stdout.strip()
    raise ValueError(f'{name} is required; install matching Clang/LLVM tools')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=REPO.parent / '.research/device-gateway-coverage')
    parser.add_argument('--clang', default='clang')
    parser.add_argument('--llvm-cov', default='llvm-cov')
    parser.add_argument('--llvm-profdata', default='llvm-profdata')
    args = parser.parse_args()
    try:
        clang, cov, profdata = map(tool, (args.clang, args.llvm_cov, args.llvm_profdata))
        output = args.build_dir.resolve()
        output.mkdir(parents=True, exist_ok=True)
        command = ['cmake', '-S', str(REPO / 'firmware'), '-B', str(output),
                   '-DCMAKE_BUILD_TYPE=Debug', f'-DCMAKE_C_COMPILER={clang}',
                   '-DCMAKE_C_FLAGS=-fprofile-instr-generate -fcoverage-mapping',
                   '-DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate']
        subprocess.run(command, check=True)
        subprocess.run(['cmake', '--build', str(output), '--parallel'], check=True)
        for profile in output.glob('*.profraw'):
            profile.unlink()
        subprocess.run(['ctest', '--test-dir', str(output), '--output-on-failure'], check=True,
                       env={**os.environ, 'LLVM_PROFILE_FILE': str(output / '%m-%p.profraw')})
        profile = output / 'coverage.profdata'
        subprocess.run([profdata, 'merge', '-sparse', *map(str, output.glob('*.profraw')),
                        '-o', str(profile)], check=True)
        data = json.loads(subprocess.check_output([cov, 'export', str(output / 'osg_gateway_test'), f'-instr-profile={profile}'], text=True))
        (output / 'coverage.json').write_text(json.dumps(data))
        targets = {str(REPO / 'firmware/src' / name) for name in
                   ('gateway_cockpit.c', 'gateway_config.c', 'gateway.c', 'gateway_registry.c',
                    'gateway_layout.c', 'gateway_publication.c')}
        measured = set()
        for source in data['data'][0]['files']:
            if source['filename'] not in targets:
                continue
            measured.add(source['filename'])
            lines = source['summary']['lines']
            print(f"{Path(source['filename']).name}: {lines['percent']:.2f}% lines "
                  f"({lines['covered']}/{lines['count']})")
            if lines['percent'] <= 70:
                raise ValueError(f"{source['filename']}: line coverage must be greater than 70%")
        if measured != targets:
            raise ValueError(f'Missing coverage for {sorted(targets - measured)}')
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'Coverage failed: {error}\n')


if __name__ == '__main__':
    main()
