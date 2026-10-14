#!/usr/bin/env python3
"""Exercise this gateway's production CDC/CAN flow against independently built JSON devices."""
import argparse
import json
import os
import re
from pathlib import Path
import select
import signal
import socket
import subprocess
import tempfile
import time
import uuid


def report_bytes(descriptor):
    """Derive each report's byte length from HID items independently of firmware."""
    raw = bytes.fromhex(descriptor)
    offset = size = count = report = 0
    bits = {}
    while offset < len(raw):
        prefix = raw[offset]
        offset += 1
        length = prefix & 3
        length = 4 if length == 3 else length
        assert prefix != 254 and offset + length <= len(raw), 'Malformed HID descriptor'
        value = int.from_bytes(raw[offset:offset + length], 'little')
        offset += length
        if prefix == 0x75:
            size = value
        elif prefix == 0x95:
            count = value
        elif prefix == 0x85:
            report = value
        elif prefix & 0xfc == 0x80:
            bits[report] = bits.get(report, 0) + size * count
    return {number: (length + 7) // 8 for number, length in bits.items()}


class Node:
    def __init__(self, arguments, directory, name, environment=None):
        self.error_path = directory / (name + '.stderr')
        self.error = self.error_path.open('w')
        self.process = subprocess.Popen(list(map(str, arguments)), stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, text=True, stderr=self.error, env=environment)
        self.sequence = 0

    def admin(self, operation):
        self.sequence += 1
        self.process.stdin.write(f'sim {self.sequence} {operation}\n')
        self.process.stdin.flush()
        assert select.select([self.process.stdout], [], [], 10)[0], f'Admin timeout: {operation}'
        line = self.process.stdout.readline()
        assert line, self.error_path.read_text()
        result = json.loads(line)
        assert result['id'] == self.sequence and result['ok'], result
        return result['simulation']

    def stop(self, abrupt=False, expected=0):
        if self.error.closed:
            return
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGKILL if abrupt else signal.SIGTERM)
        code = self.process.wait(timeout=5)
        self.error.close()
        self.process.stdin.close()
        self.process.stdout.close()
        if not abrupt:
            assert code == expected, self.error_path.read_text()

    def rejected(self, message):
        assert self.process.wait(timeout=5) != 0, 'Duplicate instance unexpectedly started'
        code = self.process.returncode
        self.stop(expected=code)
        assert message in self.error_path.read_text(), self.error_path.read_text()


def simulation_directory(namespace=None):
    if namespace is not None and re.fullmatch(r'[A-Za-z0-9._-]{1,31}', namespace) is None:
        raise ValueError('Invalid simulation namespace')
    suffix = '' if namespace is None else '-' + namespace
    return Path(f'/tmp/opensimgear-{os.geteuid()}/gateways{suffix}')


class CDC:
    def __init__(self, uid, log, namespace=None):
        self.advertisement = simulation_directory(namespace) / (uid + '.json')
        self.connection = None
        self.buffer = b''
        self.sequence = 1000
        self.epoch = 0
        self.log = log

    def close(self):
        if self.connection:
            self.connection.close()
        self.connection = None
        self.buffer = b''

    def connect(self):
        self.close()
        advertisement = json.loads(self.advertisement.read_text())
        assert advertisement['protocol'] == 3 and advertisement['role'] == 'gateway'
        port = advertisement['tcpPort']
        assert isinstance(port, int) and 0 < port <= 65535
        self.connection = socket.create_connection(('127.0.0.1', port), timeout=6)

    def line(self, command):
        if not self.connection:
            self.connect()
        self.connection.sendall((command + '\n').encode())
        while b'\n' not in self.buffer:
            chunk = self.connection.recv(16384)
            if not chunk:
                self.close()
                raise EOFError('CDC detached')
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b'\n', 1)
        reply = line.decode().strip()
        self.log.write(json.dumps({'command': command, 'reply': reply}) + '\n')
        self.log.flush()
        return reply

    def request(self, action, expected=None):
        self.sequence += 1
        route = '' if action == 'snapshot' else f'{self.epoch} '
        result = json.loads(self.line(f'cockpit {self.sequence} {route}{action}'))
        assert result['id'] == self.sequence and result['error'] == expected, (action, result)
        assert result['ok'] == (expected is None), result
        if result.get('state'):
            self.epoch = result['state']['epoch']
        return result

    def config(self, identity, boot, command, expected=None):
        return self.request(f'config {identity} {boot} {command}', expected).get('targetConfig')

    def action(self, identity, boot, command, expected=None):
        deadline = time.monotonic() + 2
        while True:
            self.sequence += 1
            result = json.loads(self.line(f'cockpit {self.sequence} {self.epoch} action {identity} {boot} {command}'))
            assert result['id'] == self.sequence
            if result.get('state'):
                self.epoch = result['state']['epoch']
            # Only reads may wait for the new lease to reach the device.
            if (expected is None and (command == 'state' or command.startswith('describe '))
                    and result['error'] == 'configuration_route_not_ready'
                    and time.monotonic() < deadline):
                time.sleep(.025)
                continue
            assert result['error'] == expected and result['ok'] == (expected is None), result
            return result.get('targetAction')

    def input_events(self, identity, boot, command, expected=None):
        deadline = time.monotonic() + 2
        while True:
            self.sequence += 1
            result = json.loads(self.line(f'cockpit {self.sequence} {self.epoch} events {identity} {boot} {command}'))
            assert result['id'] == self.sequence
            if result.get('state'):
                self.epoch = result['state']['epoch']
            if (expected is None and result['error'] == 'configuration_route_not_ready'
                    and time.monotonic() < deadline):
                time.sleep(.025)
                continue
            assert result['error'] == expected and result['ok'] == (expected is None), result
            return result.get('targetEvents')

    def until(self, predicate, timeout=8):
        deadline = time.monotonic() + timeout
        latest = None
        while time.monotonic() < deadline:
            try:
                latest = self.request('snapshot')['state']
                if predicate(latest):
                    return latest
            except (OSError, EOFError):
                self.close()
            time.sleep(.025)
        raise AssertionError(('Gateway state did not converge', latest))


def module(state, identity):
    return next(item for item in state['modules'] if item['identity'] == identity)


def acceptance(launch):
    artifacts = Path(launch['artifacts'])
    artifacts.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='gateway-', dir=artifacts))
    uid = uuid.uuid4().hex[:24]
    namespace = 'e2e-' + uuid.uuid4().hex[:24]
    environment = {**os.environ, 'OSG_SIMULATION_NAMESPACE': namespace}
    identities = [uuid.uuid4().hex for _ in range(3)]
    definitions = []
    nodes = []
    events = []
    def passed(name):
        events.append(name)
        print('PASS: ' + name, flush=True)
    for index, name in enumerate(('pedals', 'button-panel', 'shifter')):
        definition = json.loads((Path(launch['deviceRepository']) / f'simulation/configs/{name}.json').read_text())
        definition['identity'] = identities[index]
        path = directory / f'{name}.json'
        path.write_text(json.dumps(definition, indent=2) + '\n')
        definitions.append((path, definition))
    def start(arguments, name):
        node = Node(arguments, directory, name, environment)
        nodes.append(node)
        return node
    def gateway_start(state=None, name='gateway'):
        return start([launch['gatewayBinary'], '--identity', uid,
                      '--state-dir', state or directory / 'gateway-state'], name)
    def device_start(index, state=None, name=None):
        return start([launch['python'], Path(launch['deviceRepository']) / 'simulation/run.py',
                      '--config', definitions[index][0], '--gateway', uid,
                      '--state-dir', state or directory / f'device-{index}-state',
                      '--output-dir', launch['deviceBuild'], '--no-build'], name or f'device-{index}')
    log = (directory / 'cdc.jsonl').open('w')
    cdc = CDC(uid, log, namespace)
    outcome = {'ok': False, 'gateway': uid, 'devices': identities, 'namespace': namespace, 'checks': events, 'launch': launch}
    try:
        devices = [device_start(0)]
        time.sleep(.15)
        assert devices[0].process.poll() is None, devices[0].error_path.read_text()
        assert not cdc.advertisement.exists(), 'Device startup must not invent a gateway'
        gateway = gateway_start()
        state = cdc.until(lambda s: len(s['modules']) == 1 and s['modules'][0]['online'])
        assert state['modules'][0]['identity'] == identities[0]
        assert f'identity={uid}' in cdc.line('identity')
        state = cdc.until(lambda s: s['modules'][0]['published'] and not s['layoutPending'])
        first_descriptor = gateway.admin('inspect')['descriptorHex']
        devices.append(device_start(1))
        cdc.until(lambda s: len(s['modules']) == 2 and all(m['published'] for m in s['modules']) and not s['layoutPending'])
        second_descriptor = gateway.admin('inspect')['descriptorHex']
        assert second_descriptor.startswith(first_descriptor)
        devices.append(device_start(2))
        state = cdc.until(lambda s: len(s['modules']) == 3 and not s['layoutPending'] and
                          all(m['online'] and m['published'] and m['hasInput'] for m in s['modules']))
        assert len({m['sourceNode'] for m in state['modules']}) == 3
        boots = {m['identity']: m['boot'] for m in state['modules']}
        initial_sizes = {}
        for index, identity in enumerate(identities):
            active = cdc.config(identity, boots[identity], 'get_active')
            assert active['config']['name'] == definitions[index][1]['name']
            assert len(active['config']['axes']) == len(definitions[index][1]['inputs'].get('axes', []))
            assert len(active['config']['buttons']) == len(definitions[index][1]['inputs'].get('buttons', []))
            assert active['generation'] == active['savedGeneration'] == 1
            initial_sizes[index + 1] = active['layout']['reportBytes']
        assert initial_sizes == {1: 6, 2: 3, 3: 1}, initial_sizes
        cdc.until(lambda s: s['running'] and not s['layoutPending'] and all(m['published'] for m in s['modules']))
        initial_descriptor = gateway.admin('inspect')['descriptorHex']
        assert report_bytes(initial_descriptor) == initial_sizes
        passed('device-before-gateway, three independent JSON profiles and production descriptors')

        # Advertised actions share guarded routing but never change settings/HID.
        panel = identities[1]
        panel_boot = boots[panel]
        catalogue = cdc.action(panel, panel_boot, 'describe 0')
        assert catalogue['ok'] and catalogue['schemaVersion'] == 1 and catalogue['next'] is None
        actions = {item['id']: item for item in catalogue['actions']}
        assert actions['led.rgb.set_color']['parameters'][0]['type'] == 'rgb'
        assert actions['led.boolean.set_enabled']['parameters'][0]['type'] == 'boolean'
        revision = catalogue['catalogueRevision']
        settings_before = cdc.config(panel, panel_boot, 'get_active')
        led = cdc.action(panel, panel_boot, f'invoke {revision} led.rgb.set_color A010FF')
        assert led['ok'] and led['values'] == ['A010FF']
        led = cdc.action(panel, panel_boot, f'invoke {revision} led.boolean.set_enabled 1')
        assert led['ok'] and led['values'] == [True]
        for command in (f'invoke {revision} led.rgb.set_color GG00FF',
                        f'invoke {revision} led.boolean.set_enabled 2',
                        f'invoke {revision + 1} led.boolean.set_enabled 0',
                        f'invoke {revision} missing.action 0'):
            rejected = cdc.action(panel, panel_boot, command)
            assert rejected['ok'] is False and rejected['error'], rejected
        state_leds = cdc.action(panel, panel_boot, 'state')
        assert {s['id']: s['values'] for s in state_leds['states']} == {
            'led.rgb.set_color': ['A010FF'], 'led.boolean.set_enabled': [True]}
        assert devices[1].admin('inspect')['actions'] == state_leds
        assert cdc.config(panel, panel_boot, 'get_active') == settings_before
        assert gateway.admin('inspect')['descriptorHex'] == initial_descriptor
        cdc.action(panel, panel_boot + 1, 'state', 'stale_module_boot')
        gateway.admin('drop_reply 1')
        cdc.action(panel, panel_boot, f'invoke {revision} led.boolean.set_enabled 0', 'configuration_timeout')
        gateway.admin('drop_reply 0')
        assert {s['id']: s['values'] for s in cdc.action(panel, panel_boot, 'state')['states']}['led.boolean.set_enabled'] == [False]
        passed('advertised RGB/boolean actions, validation, applied state and lost-reply reconciliation preserve configuration and HID')

        event_baseline = cdc.input_events(panel, panel_boot, 'state')
        assert event_baseline['ok'] and event_baseline['schemaVersion'] == 1
        stream, after = event_baseline['stream'], event_baseline['head']
        device_before = devices[1].admin('inspect')
        devices[1].admin('contact 0 1'); devices[1].admin('contact 0 0')
        page = cdc.input_events(panel, panel_boot, f'read {stream} {after}')
        assert page['events'] == [
            {'sequence': after + 1, 'control': 0, 'pressed': True},
            {'sequence': after + 2, 'control': 0, 'pressed': False}], page
        retained_id = cdc.sequence
        devices[1].admin('contact 0 1'); devices[1].admin('contact 0 0')
        duplicate = json.loads(cdc.line(f'cockpit {retained_id} {cdc.epoch} events {panel} {panel_boot} read {stream} {after}'))
        assert duplicate['targetEvents'] == page, 'Completed transport retry must remain immutable'
        all_edges = cdc.input_events(panel, panel_boot, f'read {stream} {after}')
        assert len(all_edges['events']) == 4 and not all_edges['more']
        assert cdc.input_events(panel, panel_boot, f'read {stream + 1} 0')['error'] == 'staleStream'
        cdc.input_events(panel, panel_boot + 1, 'state', 'stale_module_boot')
        device_after = devices[1].admin('inspect')
        for field in ('settingsErases', 'settingsPrograms', 'moduleReconnects', 'descriptorHex'):
            assert device_before[field] == device_after[field], field
        passed('short panel presses survive delayed polling, immutable CAN retries and independent event cursors')

        identity = identities[0]
        boot = boots[identity]
        writer = 101
        cdc.config(identity, boot, f'claim {writer}')
        cdc.config(identity, boot, 'claim 102', 'writer_busy')
        name = 'Gateway-owned controller ✓'
        edits = [f'name {writer} {name.encode().hex()}', f'counts {writer} 4 2 1 1 1 1',
                 f'button {writer} 0 0 0 1', f'button {writer} 1 1 0 1',
                 f'button {writer} 2 24 1 0', f'button {writer} 3 10 0 0',
                 f'axis {writer} 0 0 48 100 3900 0 1 1000',
                 f'axis {writer} 1 1 49 0 4095 32768 0 1200',
                 f'hat {writer} 0 2 3 4 5', f'encoder {writer} 0 6 7', f'selector {writer} 0 8 9']
        for edit in edits:
            cdc.config(identity, boot, edit)
        draft = cdc.config(identity, boot, 'get')
        assert draft['valid'] and draft['dirty']
        assert draft['layout'] == {'buttons': 9, 'axes': 2, 'hats': 1, 'reportBytes': 7}
        assert gateway.admin('inspect')['descriptorHex'] == initial_descriptor
        cdc.config(identity, boot, f'axis {writer} 1 1 49 3000 100 32768 0 0', 'invalid_axis')
        cdc.config(identity, boot, f'apply_save {writer}')
        state = cdc.until(lambda s: module(s, identity)['configurationGeneration'] == 2 and
                          module(s, identity)['hasInput'] and not module(s, identity)['profileMatch'])
        assert not module(state, identity)['profileMatch']
        active = cdc.config(identity, boot, 'get_active')
        saved_config = active['config']
        assert saved_config['name'] == name and active['generation'] == active['savedGeneration'] == 2
        assert report_bytes(active['descriptorHex']) == {0: 7}
        gateway.admin('fault 0')
        cdc.request('update-profile ' + identity)
        failed = cdc.until(lambda s: s['layoutError'] == 'flash_save_failed')
        assert failed['layoutPending'] and gateway.admin('inspect')['descriptorHex'] == initial_descriptor
        state = cdc.until(lambda s: s['running'] and not s['layoutPending'] and module(s, identity)['profileMatch'])
        descriptor = gateway.admin('inspect')['descriptorHex']
        assert report_bytes(descriptor) == {1: 7, 2: 3, 3: 1}
        assert descriptor != initial_descriptor
        passed('routed configuration ownership, Unicode name/save, shape change and interrupted automatic gateway save')

        cdc.config(identity, boot, f'name {writer} {b"Must not survive a failed save".hex()}')
        devices[0].admin('fault 0')
        cdc.config(identity, boot, f'apply_save {writer}', 'flash_save_failed')
        assert cdc.config(identity, boot, 'get_active')['config'] == saved_config
        cdc.config(identity, boot, f'release {writer}')
        gateway.admin('drop_reply 1')
        cdc.config(identity, boot, 'get_active', 'configuration_timeout')
        gateway.admin('drop_reply 0')
        assert cdc.config(identity, boot, 'get_active')['config'] == saved_config
        passed('interrupted device save preserves active settings and CAN configuration timeout recovers')

        devices[0].admin('contact 0 1')
        devices[0].admin('contact 2 1')
        devices[0].admin('axis 0 100')
        devices[0].admin('axis 1 4095')
        devices[1].admin('contact 0 1')
        devices[2].admin('contact 2 1')
        state = cdc.until(lambda s: all(m['hasInput'] for m in s['modules']) and
                          bytes.fromhex(module(s, identity)['report'])[-4:] == b'\xff' * 4 and
                          bytes.fromhex(module(s, identities[1])['report'])[0] & 1 and
                          bytes.fromhex(module(s, identities[2])['report'])[0] & 4)
        report = bytes.fromhex(module(state, identity)['report'])
        assert report[0] & 1 and report[2] & 15 == 0
        for index, device in enumerate(devices):
            assert device.admin('inspect')['moduleReportHex'] == module(state, identities[index])['report']
        assert gateway.admin('inspect')['gatewayReports'][0]['hex'] == report.hex()
        passed('calibrated/inverted axes, buttons and hats reach all automatically enrolled controller reports')

        devices[0].admin('can_node ' + str(module(state, identities[1])['sourceNode']))
        state = cdc.until(lambda s: all(m['online'] and m['hasInput'] for m in s['modules']) and
                          len({m['sourceNode'] for m in s['modules']}) == 3)
        assert sum(device.admin('inspect')['nodeCollisions'] for device in devices) > 0
        passed('production Cyphal source-node collision repair')

        gateway_start(name='duplicate-gateway-state').rejected('Flash state is already in use')
        duplicate_state = directory / 'duplicate-gateway-state'
        gateway_start(duplicate_state, 'duplicate-gateway-identity').rejected('Gateway identity is already running')
        assert not (duplicate_state / 'registry.bin').exists()
        device_start(0, name='duplicate-device-state').rejected('Flash state is already in use')
        device_start(0, directory / 'duplicate-device-state', 'duplicate-device-identity').rejected('identity already active')
        assert len(cdc.request('snapshot')['state']['modules']) == 3
        passed('duplicate device/gateway identities and shared persistent state are rejected')

        before = gateway.admin('inspect')
        cdc.close()
        advertisement = json.loads(cdc.advertisement.read_text())
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as slow:
            slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            slow.settimeout(3)
            slow.connect(('127.0.0.1', advertisement['tcpPort']))
            slow.setblocking(False)
            assert slow.send(b'cockpit 999 snapshot\n' * 600) > 20
            time.sleep(.5)
            during = gateway.admin('inspect')
            assert during['clockMs'] > before['clockMs'] + 300 and during['canFrames'] > before['canFrames']
            assert all(device.admin('inspect')['acks'] > 0 for device in devices)
        passed('unread CDC client preserves wall-clock CAN acquisition and input acknowledgments')

        devices[0].stop(abrupt=True)
        state = cdc.until(lambda s: not module(s, identity)['online'])
        offline = module(state, identity)
        assert not offline['hasInput'] and offline['descriptorIndex'] == 0 and offline['published']
        assert bytes.fromhex(offline['report']) == b'\x00\x00\x08\x00\x00\x00\x80', offline
        assert sum(m['online'] for m in state['modules']) == 2
        devices[0] = device_start(0, name='device-0-restarted')
        state = cdc.until(lambda s: module(s, identity)['online'] and module(s, identity)['boot'] > boot and
                          module(s, identity)['profileMatch'] and module(s, identity)['hasInput'])
        new_boot = module(state, identity)['boot']
        cdc.config(identity, boot, 'get_active', 'stale_module_boot')
        restored = cdc.config(identity, new_boot, 'get_active')
        assert restored['config'] == saved_config and restored['generation'] == restored['savedGeneration'] == 2
        old_epoch = state['epoch']
        gateway.stop(abrupt=True)
        cdc.close()
        gateway = gateway_start(name='gateway-restarted')
        state = cdc.until(lambda s: s['epoch'] > old_epoch and len(s['modules']) == 3 and
                          all(m['online'] and m['profileMatch'] for m in s['modules']))
        assert not state['layoutPending'] and {m['descriptorIndex'] for m in state['modules']} == {0, 1, 2}
        assert report_bytes(gateway.admin('inspect')['descriptorHex']) == {1: 7, 2: 3, 3: 1}
        assert cdc.config(identity, new_boot, 'get_active')['config'] == saved_config
        passed('offline neutral reports, device boot/save recovery and gateway epoch/registry recovery')

        # Disable is a persisted choice, even while the module keeps announcing.
        panel = identities[1]
        cdc.request('disable ' + panel)
        state = cdc.until(lambda s: not s['layoutPending'] and not module(s, panel)['published'])
        assert not module(state, panel)['enabled'] and module(state, panel)['online']
        panel_boot = module(state, panel)['boot']
        assert cdc.action(panel, panel_boot, 'state')['ok']
        assert cdc.action(panel, panel_boot, f'invoke {revision} led.boolean.set_enabled 1')['values'] == [True]
        assert cdc.action(panel, panel_boot, 'state')['ok']
        hidden_descriptor = gateway.admin('inspect')['descriptorHex']
        devices[1].admin('contact 0 0')
        disabled_baseline = cdc.input_events(panel, panel_boot, 'state')
        devices[1].admin('direct 1')
        devices[1].admin('contact 0 1'); devices[1].admin('contact 0 0')
        disabled_events = cdc.input_events(panel, panel_boot,
            f'read {disabled_baseline["stream"]} {disabled_baseline["head"]}')
        assert [event['pressed'] for event in disabled_events['events']] == [True, False]
        assert not module(cdc.request('snapshot')['state'], panel)['hasInput']
        devices[1].admin('direct 0')
        assert report_bytes(hidden_descriptor) == {1: 7, 3: 1}
        old_epoch = state['epoch']
        gateway.stop(abrupt=True); cdc.close(); gateway = gateway_start(name='gateway-disabled-restarted')
        state = cdc.until(lambda s: s['epoch'] > old_epoch and all(m['online'] for m in s['modules']) and not s['layoutPending'])
        assert not module(state, panel)['enabled'] and not module(state, panel)['published']
        assert gateway.admin('inspect')['descriptorHex'] == hidden_descriptor
        cdc.request('enable ' + panel)
        state = cdc.until(lambda s: not s['layoutPending'] and module(s, panel)['published'])
        assert module(state, panel)['descriptorIndex'] == 2 and module(state, panel)['reportId'] == 2
        assert gateway.admin('inspect')['descriptorHex'].startswith(hidden_descriptor)
        passed('persistent disable survives announcements/restart; re-enable appends after surviving collections')

        cdc.request('forget ' + panel, 'device_online')
        devices[1].stop(abrupt=True)
        state = cdc.until(lambda s: not module(s, panel)['online'])
        assert module(state, panel)['published']
        cdc.request('forget ' + panel)
        cdc.until(lambda s: not s['layoutPending'] and all(m['identity'] != panel for m in s['modules']))
        devices[1] = device_start(1, name='device-panel-forgotten-restarted')
        state = cdc.until(lambda s: any(m['identity'] == panel and m['published'] for m in s['modules']) and not s['layoutPending'])
        assert module(state, panel)['descriptorIndex'] == 2
        passed('offline forget removes a ghost; rediscovery enrolls automatically at the end')

        for known in identities:
            cdc.request('disable ' + known)
        state = cdc.until(lambda s: not s['layoutPending'] and all(not m['enabled'] and not m['published'] and m['hasInput'] for m in s['modules']))
        assert gateway.admin('inspect')['descriptorHex'] == ''
        # CDC remains usable with zero HID collections, without a placeholder.
        assert cdc.config(identity, new_boot, 'get_active')['config'] == saved_config
        assert cdc.action(panel, module(state, panel)['boot'], 'state')['ok']
        passed('zero enabled controllers has no HID descriptor and keeps configuration over CDC')

        # Radio outputs are native firmware ports, not a frontend fixture. All
        # metadata and calibrated observations traverse the ordinary envelopes.
        radio_definition = json.loads((Path(launch['deviceRepository']) / 'simulation/configs/radio-panel.json').read_text())
        radio_id = uuid.uuid4().hex; radio_definition['identity'] = radio_id
        radio_path = directory / 'radio-panel.json'
        radio_path.write_text(json.dumps(radio_definition, indent=2) + '\n')
        radio = start([launch['python'], Path(launch['deviceRepository']) / 'simulation/run.py',
                       '--config', radio_path, '--gateway', uid, '--state-dir', directory / 'radio-state',
                       '--output-dir', launch['deviceBuild'], '--no-build'], 'radio-panel')
        devices.append(radio); identities.append(radio_id)
        state = cdc.until(lambda s: any(m['identity'] == radio_id and m['published'] for m in s['modules']) and not s['layoutPending'])
        cdc.request('disable ' + radio_id)
        state = cdc.until(lambda s: not s['layoutPending'] and not module(s, radio_id)['published'])
        radio_boot = module(state, radio_id)['boot']
        before_radio = radio.admin('inspect'); before_gateway = gateway.admin('inspect')
        catalogue = cdc.action(radio_id, radio_boot, 'describe 0')
        assert [a['display'] for a in catalogue['actions'] if 'display' in a] == [
            {'columns': 7, 'rows': 1, 'encoding': 'ascii'}, {'columns': 2, 'rows': 1, 'encoding': 'ascii'},
            {'columns': 16, 'rows': 1, 'encoding': 'utf8'}]
        revision = catalogue['catalogueRevision']
        def display(index, text):
            action = ('display.uhf.frequency.set_text', 'display.uhf.preset.set_text', 'display.text.set_text')[index]
            return cdc.action(radio_id, radio_boot, f'invoke {revision} {action} {text.encode().hex() or "-"}')
        assert display(0, '251.000')['values'] == ['251.000']
        assert display(1, '20')['values'] == ['20']
        wide = '😀' * 15 + '波'
        assert display(2, wide)['values'] == [wide]  # 63 bytes / 16 scalar cells
        assert display(0, '251.0000')['error'] == 'invalidArgument'
        assert display(0, 'é')['error'] == 'invalidArgument'
        actual = radio.admin('inspect')['displays']
        assert [d['text'] for d in actual] == ['251.000', '20', wide]
        assert [d['writes'] for d in actual] == [1, 1, 1]
        radio.admin('display_fail 0 1')
        assert display(0, '123.450')['error'] == 'failed'
        assert radio.admin('inspect')['displays'][0]['text'] == '251.000'
        radio.admin('display_fail 0 0')
        baseline = cdc.input_events(radio_id, radio_boot, 'state')
        assert baseline['contactButtonCount'] == 3 and baseline['maintainedButtons'] == [0, 1]
        assert baseline['encoderButtons'] == [3, 4, 5, 6] and baseline['selectorButtons'] == []
        radio.admin('contact 0 1'); radio.admin('contact 2 1'); radio.admin('contact 2 0')
        for pin, pressed in ((3, 1), (4, 1), (3, 0), (4, 0)):
            radio.admin(f'contact {pin} {pressed}')
        page = cdc.input_events(radio_id, radio_boot, f'read {baseline["stream"]} {baseline["head"]}')
        assert [(e['control'], e['pressed']) for e in page['events']] == [(0, True), (2, True), (2, False), (3, True), (3, False)]
        radio.admin('axis 0 3900')
        axes = cdc.input_events(radio_id, radio_boot, 'state')
        assert (axes['axisCount'], axes['axisUsage'], axes['axes'], axes['axisAvailable']) == (1, [48], [65535], [True])
        radio.admin('direct 1')
        assert cdc.input_events(radio_id, radio_boot, 'state')['axes'] == [65535]
        radio.admin('axes_available 0')
        stale = cdc.input_events(radio_id, radio_boot, 'state')
        assert stale['axisAvailable'] == [False] and stale['axes'] == stale['axisNeutral'] == [0]
        radio.admin('axes_available 1'); radio.admin('direct 0')
        after_radio = radio.admin('inspect'); after_gateway = gateway.admin('inspect')
        for field in ('settingsErases', 'settingsPrograms', 'moduleReconnects', 'descriptorHex'):
            assert before_radio[field] == after_radio[field], field
        for field in ('registryErases', 'registryPrograms', 'gatewayReconnects', 'descriptorHex'):
            assert before_gateway[field] == after_gateway[field], field
        passed('radio firmware display ports, bounded ASCII/UTF-8, independent switch/encoder events and calibrated axis freshness with HID disabled/direct USB')

        cdc.close()
        time.sleep(.1)
        assert all(node.process.poll() is None for node in [gateway, *devices])
        outcome.update(ok=True, savedConfig=saved_config, descriptorHex=descriptor, reportHex=report.hex())
        passed('closing the host connection leaves every firmware process running independently')
    finally:
        cdc.close()
        log.close()
        for node in reversed(nodes):
            if not node.error.closed:
                if node.process.poll() is None:
                    node.stop()
                else:
                    node.stop(expected=node.process.returncode)
        (directory / 'result.json').write_text(json.dumps(outcome, indent=2) + '\n')
        print(f'Evidence: {directory}', flush=True)
    assert outcome['ok']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path, required=True)
    args = parser.parse_args()
    acceptance(json.loads(args.manifest.read_text()))


if __name__ == '__main__':
    main()
