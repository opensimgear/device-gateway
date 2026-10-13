#!/usr/bin/env python3
"""Build if needed, then start a gateway independently of Cockpit or device processes."""
import argparse
import os
from pathlib import Path
import sys

sys.dont_write_bytecode = True
from build import REPOSITORY, build


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--identity', default='00112233445566778899aabb', metavar='UID24')
    parser.add_argument('--state-dir', type=Path)
    parser.add_argument('--common-source', type=Path)
    parser.add_argument('--rebuild', action='store_true')
    args = parser.parse_args()
    binary = REPOSITORY / '.simulation' / 'build' / 'osg-gateway-sim'
    if args.rebuild or not binary.is_file():
        binary = build(common=args.common_source)
    state = (args.state_dir or REPOSITORY / '.simulation' / 'state' / args.identity).resolve()
    os.execv(str(binary), [str(binary), '--identity', args.identity, '--state-dir', str(state)])


if __name__ == '__main__':
    main()
