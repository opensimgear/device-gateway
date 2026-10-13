#!/usr/bin/env python3
"""Build the native gateway with the production firmware core and its pinned dependency."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess

REPOSITORY = Path(__file__).resolve().parents[1]
COMMON_URL = 'https://github.com/opensimgear/firmware-common.git'


def common_source(override=None):
    if override is not None:
        source = override.resolve()
    else:
        source = REPOSITORY / '.simulation' / 'deps' / 'firmware-common'
        revision_file = REPOSITORY / 'firmware' / 'cmake' / 'common-revision.cmake'
        revision = re.search(r'set\(OSG_COMMON_REVISION ([0-9a-f]{40})\)', revision_file.read_text()).group(1)
        if not source.exists():
            source.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run(['git', 'clone', '--no-checkout', COMMON_URL, str(source)], check=True)
            subprocess.run(['git', '-C', str(source), 'checkout', '--detach', revision], check=True)
        actual = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != revision:
            raise SystemExit(f'{source} is at {actual}; expected {revision}. Remove the cached checkout or use --common-source.')
        subprocess.run(['git', '-C', str(source), 'submodule', 'update', '--init', '--recursive'], check=True)
    if not (source / 'src' / 'config' / 'line.c').is_file():
        raise SystemExit(f'Invalid firmware-common source: {source}')
    return source


def build(output_dir=None, common=None, sanitize=False):
    compiler = shutil.which(os.environ.get('CC', 'cc'))
    if not compiler:
        raise SystemExit('A C11 compiler (cc, or CC override) is required.')
    common = common_source(common)
    firmware = REPOSITORY / 'firmware'
    cyphal = firmware / 'shared-cyphal'
    output = (output_dir or REPOSITORY / '.simulation' / 'build').resolve() / 'osg-gateway-sim'
    output.parent.mkdir(parents=True, exist_ok=True)
    sources = [Path(__file__).with_name('gateway.c'),
               *[firmware / 'src' / name for name in ('gateway.c', 'gateway_registry.c', 'gateway_journal.c',
                  'gateway_cockpit.c', 'gateway_config.c', 'gateway_layout.c')],
               common / 'src' / 'config' / 'line.c', cyphal / 'src' / 'cyphal.c',
               cyphal / 'src' / 'gateway_protocol.c', cyphal / 'vendor' / 'libcanard' / 'canard.c']
    includes = [firmware / 'include', common / 'include', cyphal / 'include', cyphal / 'generated',
                cyphal / 'vendor' / 'libcanard']
    for source in sources:
        if not source.is_file():
            raise SystemExit(f'Required source missing: {source}')
    flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-UNDEBUG']
    if sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run([compiler, *flags,
                    *[arg for path in includes for arg in ('-I', str(path))],
                    *map(str, sources), '-o', str(output)], check=True)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', type=Path)
    parser.add_argument('--common-source', type=Path)
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    print(build(args.output_dir, args.common_source, args.sanitize))


if __name__ == '__main__':
    main()
