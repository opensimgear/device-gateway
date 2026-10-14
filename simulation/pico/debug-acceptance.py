#!/usr/bin/env python3
"""Cross-process Pico firmware -> native Cockpit -> DCS peer acceptance.

Requires the firmware host harness (firmware/test.mjs), a debug build of
osg-gateway-service and a Cockpit checkout for its authored DCS test peer.
Every process, settings file and discovery namespace is isolated by this runner.
"""
import argparse
import json
import os
from pathlib import Path
import selectors
import socket
import subprocess
import time
import uuid

REPOSITORY = Path(__file__).resolve().parents[2]


def line(process, timeout=15):
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        if not selector.select(timeout):
            raise TimeoutError(f'No reply from process {process.pid}')
    value = process.stdout.readline()
    if not value:
        raise RuntimeError(f'Process {process.pid} closed unexpectedly')
    return json.loads(value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--service', type=Path, required=True)
    parser.add_argument('--cockpit-source', type=Path, required=True)
    args = parser.parse_args()
    namespace = f'pico-{uuid.uuid4().hex[:12]}'
    directory = REPOSITORY / '.research/pico/acceptance' / namespace
    directory.mkdir(parents=True)
    env = {**os.environ, 'OSG_SIMULATION_NAMESPACE': namespace, 'OSG_LOCAL_GATEWAYS_ONLY': '1',
           'OSG_DCS_BIOS_CONFIG': str(directory / 'dcs-settings.json')}
    children = []
    def start(command):
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1, env=env)
        children.append(process)
        return process
    sequence = 0
    def call(command, expected=True, **fields):
        nonlocal sequence
        sequence += 1
        service.stdin.write(json.dumps({'command': command, 'clientId': 'pico-acceptance-client', **fields}) + '\n')
        service.stdin.flush()
        result = line(service)
        assert result['ok'] is expected, f'{command}: {result.get("error")}'
        return result.get('state', {})
    def poll(predicate, command='status', timeout=8, **fields):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            state = call(command, **fields)
            if predicate(state):
                return state
            time.sleep(0.04)
        diagnostic = {'dcsBios': state.get('dcsBios'), 'moduleActions': state.get('moduleActions')}
        raise AssertionError(f'Timed out waiting for {command}: {diagnostic}')
    peer_sequence = 0
    def admin(command):
        nonlocal peer_sequence
        peer_sequence += 1
        peer.stdin.write(f'sim {peer_sequence} {command}\n')
        peer.stdin.flush()
        reply = line(peer)
        assert reply['ok'], reply
        return reply['simulation']
    try:
        fixture = start(['python3', str(Path(__file__).with_name('debug-fixture.py')), '--namespace', namespace])
        ready = line(fixture)
        gateway_id = ready['gatewayId']
        service = start([str(args.service.resolve())])
        inventory = call('ports')
        port = next(port for port in inventory['ports'] if port['gatewayId'] == gateway_id)
        assert len(inventory['ports']) == 1, 'Acceptance must not open physical serial ports'
        state = call('connect', gatewayId=gateway_id, backend='pico', port=port['path'])
        assert state['backend'] == 'pico' and state['protocolVersion'] == 3 and state['lifecycle'] == 'automatic'
        target = {'gatewayId': gateway_id}
        state = poll(lambda state: not state['layoutPending'], **target)
        def device(preset):
            return next(device for device in state['debug']['devices'] if device['preset'] == preset)
        def inject(device, operation, values=None):
            return call('debug_invoke', moduleId=device['id'], boot=device['boot'], debugOperation=operation,
                        debugArgs=values or [], **target)
        pedals = device('pedals')
        inject(pedals, 'axis', [0, 2048])
        state = poll(lambda state: 32000 < next(m for m in state['modules'] if m['id'] == pedals['id'])['axes'][0] < 34000, **target)
        shifter = device('shifter')
        inject(shifter, 'contact', [0, 1])
        poll(lambda state: 1 in next(m for m in state['modules'] if m['id'] == shifter['id'])['buttons'], **target)
        inject(shifter, 'contact', [0, 0])
        for preset in ['master-caution', 'radio', 'all-inputs']:
            call('debug_invoke', debugOperation='add', preset=preset, debugArgs=[], **target)
            state = poll(lambda state: not state['layoutPending'] and any(d['preset'] == preset for d in state['debug']['devices']), **target)
        all_inputs = device('all-inputs')
        module = next(m for m in state['modules'] if m['id'] == all_inputs['id'])
        assert (module['buttonCount'], module['axisCount'], module['hatCount']) == (128, 8, 4)
        inject(all_inputs, 'hat', [0, 3]); inject(all_inputs, 'selector', [0, 2]); inject(all_inputs, 'contact', [142, 1])
        poll(lambda state: next(m for m in state['modules'] if m['id'] == all_inputs['id'])['hats'][0] == 3, **target)
        inject(all_inputs, 'neutral')
        radio = device('radio'); caution = device('master-caution')
        for device in [radio, caution]:
            call('config_read', moduleId=device['id'], **target)
            call('action_describe', moduleId=device['id'], **target)
            call('events_state', moduleId=device['id'], **target)
            call('disable', moduleId=device['id'], **target)
            state = poll(lambda state: not state['layoutPending'] and not next(m for m in state['modules'] if m['id'] == device['id'])['published'], **target)
        config_state = call('config_read', moduleId=radio['id'], **target)
        config = config_state['moduleConfigs'][radio['id']]['config']
        call('claim', moduleId=radio['id'], **target)
        config['name'] = 'Pico acceptance radio'
        config['axes'][0][5] = 1
        call('config_apply', moduleId=radio['id'], config=config, **target)
        time.sleep(0.05)  # The renewed route first receives fresh device samples.
        call('config_read', moduleId=radio['id'], **target)
        call('release', moduleId=radio['id'], **target)
        inject(radio, 'neutral')
        state = poll(lambda state: not state['layoutPending'], **target)
        event_state = call('events_state', moduleId=radio['id'], **target)['moduleEvents'][radio['id']]
        assert event_state['axes'][0] == 0, 'Inverted neutral must reach the declared neutral'
        sockets = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in range(2)]
        for sock in sockets:
            sock.bind(('127.0.0.1', 0))
        command_port, export_port = [sock.getsockname()[1] for sock in sockets]
        for sock in sockets:
            sock.close()
        peer = start(['node', str(args.cockpit_source / 'simulation/dcs-bios/peer.mjs'),
                      '--command-port', str(command_port), '--export-port', str(export_port)])
        catalogue = str((args.cockpit_source / 'simulation/dcs-bios/catalogue').resolve())
        def binding(id, device, **fields):
            return {'id': id, 'gatewayId': gateway_id, 'moduleId': device['id'], **fields}
        profile = {'schemaVersion': 2, 'aircraftModule': 'A-10C', 'cataloguePath': catalogue,
                   'commandHost': '127.0.0.1', 'commandPort': command_port, 'listenAddress': '127.0.0.1',
                   'listenPort': export_port, 'multicastGroup': '', 'multicastInterface': '127.0.0.1', 'freshnessMs': 200,
                   'inputs': [binding('caution', caution, kind='button', control=0, command='UFC_MASTER_CAUTION', interface='set_state'),
                              binding('test', radio, kind='button', control=2, command='UHF_TEST', interface='set_state'),
                              binding('switch', radio, kind='switch', control=0, command='UHF_SQUELCH', interface='set_state'),
                              binding('preset', radio, kind='encoder', control=0, command='UHF_PRESET_SEL', interface='fixed_step'),
                              binding('mhz', radio, kind='encoder', control=1, command='UHF_10MHZ_SEL', interface='fixed_step'),
                              binding('volume', radio, kind='axis', control=0, command='UHF_VOL', interface='set_state', deadband=64)],
                   'outputs': [binding('lamp', caution, control='MASTER_CAUTION', actionId='led.status.set_mode', format={'kind': 'boolean'}),
                               binding('frequency', radio, control='UHF_FREQUENCY', actionId='display.frequency.set_text', format={'kind': 'text'}),
                               binding('preset-display', radio, control='UHF_PRESET', actionId='display.preset.set_text', format={'kind': 'text'}),
                               binding('volume-display', radio, control='UHF_VOL', actionId='display.text.set_text', format={'kind': 'fixed', 'scale': 100 / 65535, 'decimals': 1, 'prefix': 'VOL '})]}
        call('dcs_configure', dcsConfig=profile)
        call('dcs_start')
        poll(lambda state: state['dcsBios']['status'] == 'active', command='dcs_status')
        admin('warning 1')
        poll(lambda state: state['moduleActions'][caution['id']]['states'][0]['values'] == ['on'], command='action_state', moduleId=caution['id'], **target)
        inject(caution, 'contact', [0, 1]);time.sleep(0.06);inject(caution, 'contact', [0, 0])
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and admin('inspect')['warning']:
            time.sleep(0.05)
        assert not admin('inspect')['warning']
        inject(radio, 'contact', [0, 1]);inject(radio, 'turn', [0, 2]);inject(radio, 'turn', [1, -1]);inject(radio, 'axis', [0, 2000])
        inject(radio, 'contact', [2, 1]);time.sleep(0.06);inject(radio, 'contact', [2, 0]);time.sleep(0.5)
        commands = [record['line'] for record in admin('inspect')['commands']]
        for prefix in ['UFC_MASTER_CAUTION 1', 'UFC_MASTER_CAUTION 0', 'UHF_TEST 1', 'UHF_TEST 0', 'UHF_SQUELCH 1', 'UHF_PRESET_SEL INC', 'UHF_10MHZ_SEL DEC', 'UHF_VOL ']:
            assert any(command.startswith(prefix) for command in commands), (prefix, commands)
        assert commands.count('UHF_PRESET_SEL INC') == 2, commands
        admin('frequency 123.450');admin('preset 20');admin('volume 32768')
        def displays(state):
            values = {entry['id']: entry['values'][0] for entry in state['moduleActions'][radio['id']]['states']}
            return values.get('display.frequency.set_text') == '123.450' and values.get('display.preset.set_text') == '20' and values.get('display.text.set_text', '').strip() == 'VOL 50.0'
        poll(displays, command='action_state', moduleId=radio['id'], **target)
        admin('pause 1')
        poll(lambda state: not state['dcsBios']['feedbackFresh'] and all(output['status'] == 'stale' for output in state['dcsBios']['outputs']), command='dcs_status')
        poll(lambda state: all(not entry['values'][0].strip() for entry in state['moduleActions'][radio['id']]['states'] if entry['id'].startswith('display.')), command='action_state', moduleId=radio['id'], **target)
        admin('pause 0')
        poll(lambda state: state['dcsBios']['status'] == 'active', command='dcs_status')
        inject(radio, 'reboot')
        call('debug_invoke', expected=False, moduleId=radio['id'], boot=radio['boot'], debugOperation='neutral', debugArgs=[], **target)
        poll(lambda state: state['dcsBios']['status'] == 'active', command='dcs_status')
        call('dcs_stop')
        assert not call('dcs_status')['dcsBios']['running']
        report = {'passed': True, 'namespace': namespace, 'gatewayId': gateway_id, 'commands': commands,
                  'features': ['raw inputs', '128 buttons/8 axes/4 hats', 'config/calibration', 'HID disabled event acquisition',
                               'DCS momentary/switch/encoder/axis commands', 'LED/display feedback', 'stale fallback', 'reboot/stale boot guard']}
        (directory / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps({'passed': True, 'report': str(directory / 'report.json')}))
    finally:
        for process in reversed(children):
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    main()
