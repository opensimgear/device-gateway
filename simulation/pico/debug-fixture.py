#!/usr/bin/env python3
"""Expose the Pico debug firmware host harness to ordinary Cockpit discovery.

This developer fixture owns its firmware process. It uses the same portable
firmware parser/acquisition/lifecycle/output code; USB electrical enumeration
and flash are qualified by the physical board and the separate C tests.
"""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import socket
import stat
import subprocess

REPOSITORY = Path(__file__).resolve().parents[2]
SERIAL = 'OSG-PICO-0123456789abcdef'


def private_directory(path):
    path.mkdir(mode=0o700, exist_ok=True)
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) != 0o700:
        raise ValueError(f'{path}: must be a private directory owned by the current user')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--namespace', default=os.environ.get('OSG_SIMULATION_NAMESPACE', 'pico-debug'))
    parser.add_argument('--harness', type=Path, default=REPOSITORY / '.research/pico/host/osg-debug-stdio')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9._-]{1,31}', args.namespace):
        parser.error('Namespace needs 1..31 ASCII letters, digits, dots, underscores or hyphens')
    if not args.harness.is_file():
        parser.error('Build the host harness with firmware/test.mjs first')
    root = Path(f'/tmp/opensimgear-{os.getuid()}')
    private_directory(root)
    directory = root / f'gateways-{args.namespace}'
    private_directory(directory)
    advertisement = directory / f'{SERIAL}.json'
    if advertisement.exists():
        parser.error(f'{advertisement} already exists; use another namespace')
    process = subprocess.Popen([str(args.harness.resolve())], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.bind(('127.0.0.1', 0))
    server.listen(4)
    server.settimeout(0.5)
    stopped = False
    def stop(_signal, _frame):
        nonlocal stopped
        stopped = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    def request(line):
        process.stdin.write(line + '\n')
        process.stdin.flush()
        response = process.stdout.readline(65537)
        if not response or len(response) > 65536:
            raise RuntimeError('Firmware harness closed or exceeded its response limit')
        return response.encode()
    payload = {'protocol': 3, 'role': 'gateway', 'gatewayType': 'pico-debug-gateway', 'serialNumber': SERIAL,
               'manufacturer': 'OpenSimGear', 'friendlyName': 'Pico debug gateway', 'endpoint': f'{SERIAL}.sock',
               'pid': os.getpid(), 'tcpPort': server.getsockname()[1]}
    descriptor = os.open(advertisement, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, 'w') as file:
            json.dump(payload, file)
        print(json.dumps({'ready': True, 'namespace': args.namespace, 'gatewayId': f'pico:{SERIAL}', 'port': server.getsockname()[1]}), flush=True)
        while not stopped and process.poll() is None:
            try:
                connection, _ = server.accept()
            except socket.timeout:
                continue
            with connection:
                connection.settimeout(0.5)
                request('transport 1')
                buffer = bytearray()
                try:
                    while not stopped:
                        try:
                            data = connection.recv(1024)
                        except socket.timeout:
                            continue
                        if not data:
                            break
                        buffer.extend(data)
                        if len(buffer) > 1024 and b'\n' not in buffer:
                            break
                        while b'\n' in buffer:
                            line, _, buffer = buffer.partition(b'\n')
                            if len(line) >= 1024:
                                raise ValueError('Oversized CDC request')
                            connection.sendall(request(line.rstrip(b'\r').decode('ascii')))
                except (ConnectionError, ValueError, UnicodeError):
                    pass
                finally:
                    request('transport 0')
    finally:
        advertisement.unlink(missing_ok=True)
        server.close()
        process.stdin.close()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


if __name__ == '__main__':
    main()
