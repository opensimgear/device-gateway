#!/usr/bin/env python3
"""Bootstrap external repositories and test the gateway's complete CAN/CDC flow."""
import argparse
import json
import os
from pathlib import Path
import sys

sys.dont_write_bytecode = True
from bootstrap import Dependencies, command, source_arguments


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', action='append', default=[], metavar='NAME=PATH')
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--with-cockpit', action='store_true', help='Also clone/build Cockpit and run its native service acceptance')
    parser.add_argument('--prepare-only', action='store_true')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    dependencies = Dependencies(root, source_arguments(args.source))
    device = dependencies.acquire('device-template')
    common = dependencies.common(root, device)
    build = root / '.e2e/build'
    for name, repository in [('device-gateway', root), ('device-template', device)]:
        command([sys.executable, repository / 'simulation/build.py', '--common-source', common,
                 '--output-dir', build / name, *(['--sanitize'] if args.sanitize else [])])
    configuration = {
        'root': str(root), 'gatewayRepository': str(root), 'deviceRepository': str(device),
        'gatewayBinary': str(build / 'device-gateway/osg-gateway-sim'),
        'deviceBuild': str(build / 'device-template'), 'commonSource': str(common),
        'python': sys.executable, 'artifacts': str(root / '.e2e/runs'),
    }
    if args.with_cockpit:
        cockpit = dependencies.acquire('cockpit')
        command(['pnpm', 'install', '--frozen-lockfile'], cwd=cockpit)
        command(['cargo', 'build', '--locked', '--manifest-path', cockpit / 'src-tauri/Cargo.toml', '--bin', 'osg-gateway-service'],
                env=dict(os.environ, CARGO_TARGET_DIR=str(build / 'cockpit')))
        configuration.update(cockpitRepository=str(cockpit), serviceBinary=str(build / 'cockpit/debug/osg-gateway-service'))
    manifest = root / '.e2e/launch.json'
    manifest.write_text(json.dumps(configuration, indent=2) + '\n')
    print(f'Launch manifest: {manifest}', flush=True)
    if not args.prepare_only:
        command([sys.executable, root / 'tests/e2e/acceptance.py', '--manifest', manifest])
        if args.with_cockpit:
            command(['node', '--test', cockpit / 'tests/e2e/acceptance.mjs'], env=dict(os.environ, OSG_E2E_MANIFEST=str(manifest)))


if __name__ == '__main__':
    main()
