#!/usr/bin/env python3
"""Expose the Pico debug firmware host harness to ordinary Cockpit discovery.

This developer fixture owns its firmware process. It uses the same portable
firmware parser/acquisition/lifecycle/output code; USB electrical enumeration
and flash are qualified by the physical board and the separate C tests.
"""
import argparse
from collections import deque
import json
import os
from pathlib import Path
import re
import selectors
import signal
import socket
import stat
import subprocess
import time

REPOSITORY = Path(__file__).resolve().parents[2]
SERIAL = 'OSG-PICO-0123456789abcdef'
MAX_LINE = 65536


class FirmwarePipe:
    """Read bounded firmware lines without assuming each command has a reply."""
    def __init__(self, process):
        self.process = process
        self.selector = selectors.DefaultSelector()
        self.selector.register(process.stdout, selectors.EVENT_READ)
        os.set_blocking(process.stdout.fileno(), False)
        self.buffer = bytearray()
        self.lines = deque()

    def read_ready(self, timeout):
        if self.lines:
            return
        if not self.selector.select(timeout):
            return
        chunk = os.read(self.process.stdout.fileno(), 8192)
        if not chunk:
            raise RuntimeError('Firmware harness closed stdout')
        self.buffer.extend(chunk)
        while b'\n' in self.buffer:
            line, _, remainder = self.buffer.partition(b'\n')
            self.buffer = bytearray(remainder)
            if len(line) + 1 > MAX_LINE:
                raise RuntimeError('Firmware harness exceeded its line limit')
            self.lines.append(bytes(line) + b'\n')
        if len(self.buffer) >= MAX_LINE:
            raise RuntimeError('Firmware harness exceeded its line limit')

    def readline(self, timeout):
        deadline = time.monotonic() + timeout
        while not self.lines:
            if self.process.poll() is not None:
                raise RuntimeError('Firmware harness exited unexpectedly')
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('Firmware harness did not reply')
            self.read_ready(remaining)
        return self.lines.popleft()

    def drain(self):
        self.lines.clear()
        while self.selector.select(0):
            self.read_ready(0)
            self.lines.clear()


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
    process = subprocess.Popen([str(args.harness.resolve())], stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
    firmware = FirmwarePipe(process)
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
    def transport(connected):
        process.stdin.write(f'transport {int(connected)}\n'.encode('ascii'))
        while True:
            response = firmware.readline(5)
            if response == b'{"transport":true}\n':
                return
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
                transport(True)
                buffer = bytearray()
                try:
                    with selectors.DefaultSelector() as selector:
                        selector.register(connection, selectors.EVENT_READ)
                        selector.register(process.stdout, selectors.EVENT_READ)
                        while not stopped:
                            for ready, _ in selector.select(0.5):
                                if ready.fileobj is process.stdout:
                                    firmware.read_ready(0)
                                    while firmware.lines:
                                        connection.sendall(firmware.lines.popleft())
                                    continue
                                data = connection.recv(1024)
                                if not data:
                                    raise ConnectionError('CDC client disconnected')
                                buffer.extend(data)
                                while b'\n' in buffer:
                                    line, _, buffer = buffer.partition(b'\n')
                                    if len(line) >= 1024 or line.startswith(b'transport '):
                                        raise ValueError('Invalid CDC request')
                                    process.stdin.write(line + b'\n')
                                if len(buffer) >= 1024:
                                    raise ValueError('Oversized CDC request')
                except (ConnectionError, TimeoutError, ValueError):
                    pass
                finally:
                    transport(False)
                    # A publication already buffered by the subprocess belongs
                    # to the old USB session and must not reach a later socket.
                    firmware.drain()
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
