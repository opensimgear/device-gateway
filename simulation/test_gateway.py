#!/usr/bin/env python3
"""Exercise the gateway simulation boundary; production CAN/device flows live in E2E."""
import argparse
import json
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import uuid

sys.dont_write_bytecode = True
from build import REPOSITORY, build

BINARY = None
ARTIFACTS = None


class Node:
    def __init__(self, uid, state, name):
        self.uid = uid
        self.advertisement = Path(f'/tmp/opensimgear-{os.geteuid()}/gateways/{uid}.json')
        self.bus_advertisement = self.advertisement.with_suffix('.can.json')
        self.error_path = ARTIFACTS / f'{name}.stderr'
        self.error = self.error_path.open('w')
        self.process = subprocess.Popen([str(BINARY), '--identity', uid, '--state-dir', str(state)],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, stderr=self.error)
        self.sequence = 0

    def ready(self):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise AssertionError(self.error_path.read_text())
            try:
                ad = json.loads(self.advertisement.read_text())
                if ad['pid'] == self.process.pid:
                    return ad
            except (FileNotFoundError, json.JSONDecodeError):
                pass
            time.sleep(.01)
        raise AssertionError('Gateway did not become ready: ' + self.error_path.read_text())

    def admin(self, operation):
        self.sequence += 1
        self.process.stdin.write(f'sim {self.sequence} {operation}\n')
        self.process.stdin.flush()
        assert select.select([self.process.stdout], [], [], 5)[0], operation
        result = json.loads(self.process.stdout.readline())
        assert result['id'] == self.sequence and result['ok'], result
        return result['simulation']

    def stop(self, abrupt=False, expected=0):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGKILL if abrupt else signal.SIGTERM)
        result = self.process.wait(timeout=5)
        self.error.close()
        self.process.stdin.close()
        self.process.stdout.close()
        if not abrupt:
            assert result == expected, self.error_path.read_text()

    def connect(self):
        return socket.create_connection(('127.0.0.1', self.ready()['tcpPort']), timeout=3)


def command(connection, text):
    connection.sendall((text + '\n').encode())
    result = bytearray()
    while not result.endswith(b'\n'):
        data = connection.recv(1)
        if not data:
            raise EOFError('Gateway detached')
        result.extend(data)
    return result.decode().strip()


class GatewayBoundaryTest(unittest.TestCase):
    def setUp(self):
        self.uid = uuid.uuid4().hex[:24]
        self.state = ARTIFACTS / self.id().split('.')[-1]
        self.nodes = []
        self.node = self.start(self.uid, self.state, self.id().split('.')[-1])
        self.node.ready()

    def start(self, uid, state, name):
        node = Node(uid, state, name)
        self.nodes.append(node)
        return node

    def tearDown(self):
        for node in reversed(self.nodes):
            if not node.error.closed:
                node.stop()

    def test_normal_protocol_empty_gateway(self):
        ad = self.node.ready()
        self.assertEqual(ad['serialNumber'], self.uid)
        self.assertEqual(ad['role'], 'gateway')
        self.assertEqual(ad['protocol'], 2)
        self.assertEqual(self.node.advertisement.stat().st_mode & 0o777, 0o600)
        with self.node.connect() as cdc:
            identity = command(cdc, 'identity')
            self.assertIn(f'identity={self.uid}', identity)
            self.assertIn('protocol=2', identity)
            snapshot = json.loads(command(cdc, 'cockpit 17 snapshot'))
            self.assertTrue(snapshot['ok'])
            self.assertEqual(snapshot['id'], 17)
            self.assertEqual(snapshot['state']['modules'], [])
            stale = json.loads(command(cdc, 'cockpit 18 1 apply'))
            self.assertEqual(stale['error'], 'stale_epoch')
            self.assertEqual(json.loads(command(cdc, 'sim 19 reboot'))['error'], 'invalid_request')
            self.assertEqual(json.loads(command(cdc, 'x' * 4096))['error'], 'invalid_request')

    def test_duplicate_identity_cannot_write_flash(self):
        duplicate_state = self.state.with_name(self.state.name + '-duplicate')
        duplicate = self.start(self.uid, duplicate_state, 'duplicate-identity')
        duplicate.process.wait(timeout=5)
        duplicate.stop(expected=1)
        self.assertIn('Gateway identity is already running', duplicate.error_path.read_text())
        self.assertFalse((duplicate_state / 'registry.bin').exists())
        self.assertEqual(self.node.admin('inspect')['gatewayEpoch'], 65536)

    def test_duplicate_state_cannot_write_flash(self):
        duplicate = self.start(uuid.uuid4().hex[:24], self.state, 'duplicate-state')
        duplicate.process.wait(timeout=5)
        duplicate.stop(expected=1)
        self.assertIn('Flash state is already in use', duplicate.error_path.read_text())
        self.assertEqual(self.node.admin('inspect')['gatewayEpoch'], 65536)

    def test_independent_clock_client_exit_and_crash_restart(self):
        with self.node.connect() as cdc:
            first = json.loads(command(cdc, 'cockpit 1 snapshot'))['state']['epoch']
        before = self.node.admin('inspect')['clockMs']
        time.sleep(.04)
        self.assertGreater(self.node.admin('inspect')['clockMs'], before)
        self.node.stop(abrupt=True)
        restarted = self.start(self.uid, self.state, 'restart')
        restarted.ready()
        with restarted.connect() as cdc:
            epoch = json.loads(command(cdc, 'cockpit 2 snapshot'))['state']['epoch']
        self.assertGreater(epoch, first)
        restarted.stop()
        self.assertFalse(restarted.advertisement.exists())
        self.assertFalse(restarted.bus_advertisement.exists())

    def test_udp_admission_duplicate_identity_and_malformed_frames(self):
        broker = json.loads(self.node.bus_advertisement.read_text())
        target = ('127.0.0.1', broker['port'])
        identity = uuid.uuid4().bytes
        packet = bytearray(92)
        packet[:6] = b'OSGB\x01\x01'
        packet[12:28] = identity
        struct.pack_into('<I', packet, 28, os.getpid())
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as first, socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as second:
            first.settimeout(3)
            second.settimeout(3)
            first.sendto(packet, target)
            response = first.recv(128)
            self.assertEqual(response[:6], b'OSGB\x01\x02')
            self.assertEqual(response[28], 0)
            second.sendto(packet, target)
            self.assertEqual(second.recv(128)[28], 1)
            initial = self.node.admin('inspect')['canFrames']
            packet[5] = 3
            packet[7] = 65
            first.sendto(packet, target)
            first.sendto(b'OSGB\x01\x03', target)
            time.sleep(.03)
            self.assertEqual(self.node.admin('inspect')['canFrames'], initial)
        with self.node.connect() as cdc:
            self.assertEqual(json.loads(command(cdc, 'cockpit 1 snapshot'))['state']['modules'], [])

    def test_unread_client_does_not_block_clock_or_shutdown(self):
        cdc = self.node.connect()
        cdc.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        cdc.setblocking(False)
        request = b'cockpit 1 snapshot\n' * 20000
        deadline = time.monotonic() + 1
        while request and time.monotonic() < deadline:
            try:
                sent = cdc.send(request)
                request = request[sent:]
            except BlockingIOError:
                break
        time.sleep(.5)
        before = self.node.admin('inspect')['clockMs']
        time.sleep(.03)
        self.assertGreater(self.node.admin('inspect')['clockMs'], before)
        cdc.close()
        self.node.stop()
        self.assertFalse(self.node.advertisement.exists())


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path)
    parser.add_argument('--common-source', type=Path)
    args = parser.parse_args()
    BINARY = args.binary.resolve() if args.binary else build(common=args.common_source, sanitize=True)
    parent = REPOSITORY / '.simulation' / 'test-runs'
    parent.mkdir(parents=True, exist_ok=True)
    ARTIFACTS = Path(tempfile.mkdtemp(prefix='gateway-', dir=parent))
    print(f'Artifacts: {ARTIFACTS}', flush=True)
    unittest.main(argv=[sys.argv[0]], verbosity=2)
