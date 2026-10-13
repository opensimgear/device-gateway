// End-to-end HTTP/serial/protocol tests use the actual production C state machine.
// The transport emulates USB attachment; these tests do not verify physical USB HID.
// First build the host harness: node simulation/pico/firmware/test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { Readable } from 'node:stream';
import { setTimeout as delay } from 'node:timers/promises';
import { picoGateway } from './pico-bridge.mjs';
import { createProtocolFixture } from './pico-protocol-fixture.mjs';

const clientA = 'protocol-test-a-0123456789';
const clientB = 'protocol-test-b-0123456789';
const path = '/dev/cu.usbmodemOSGTEST';
const shifter = 'OSG-SQ-0042';
const moduleById = (result, id) => result.state.modules.find(module => module.id === id);

function harness() {
  const fixture = createProtocolFixture();
  const server = new EventEmitter();
  let middleware;
  picoGateway({ ...fixture, reconnectDelay: 5, reconnectPoll: 5, heartbeatInterval: 1000 }).configureServer({
    httpServer: server, middlewares: { use(_path, handler) { middleware = handler; } },
  });
  async function request(command, extra = {}, clientId = clientA) {
    const req = Readable.from([Buffer.from(JSON.stringify({ command, clientId, ...extra }))]);
    Object.assign(req, { method: 'POST', url: '/', socket: { remoteAddress: '127.0.0.1' },
      headers: { host: '127.0.0.1:5173', origin: 'http://127.0.0.1:5173',
        'content-type': 'application/json', 'x-osg-simulator': '1' } });
    let result;
    const res = { statusCode: 200, setHeader() {}, end(body) { result = JSON.parse(body); } };
    await middleware(req, res);
    assert.ok(result);
    return { status: res.statusCode, ...result };
  }
  async function close() { server.emit('close'); await fixture.close(); }
  return { request, close, fixture, connect: () => request('connect', { port: path }) };
}

test('production C protocol integrates with Cockpit input, setup leases, Unicode, and USB layout changes', async () => {
  const h = harness();
  try {
    const initial = await h.request('status');
    assert.equal(initial.state.connected, false);
    assert.deepEqual(initial.state.modules, []);
    const connected = await h.connect();
    assert.equal(connected.ok, true, connected.error);
    assert.equal(connected.state.backend, 'pico');
    assert.equal(connected.state.gatewaySerial, 'OSG-PICO-MOCK-0123456789ABCDEF');
    assert.equal(connected.state.running, false);
    assert.equal(connected.state.modules.length, 4);
    assert.deepEqual(connected.state.modules.map(module => module.settings.label), ['OSG Sequential shifter', 'OSG Pedal set', 'OSG Button panel', 'OSG Handbrake']);
    assert.ok(connected.state.modules.every(module => module.registered && !module.active));
    assert.equal((await h.request('input', { moduleId: shifter, buttons: [1] })).ok, false);
    assert.equal((await h.request('start')).state.running, true);
    const pressed = await h.request('input', { moduleId: shifter, buttons: [1, 3] });
    assert.equal(pressed.ok, true, pressed.error);
    assert.deepEqual(moduleById(pressed, shifter).buttons, [1, 3]);
    await delay(15); // Let the production debounce deadline elapse before the next tick.
    assert.deepEqual(moduleById(await h.request('status'), shifter).buttons, [1, 3]);
    const invalidButtons = await h.request('input', { moduleId: shifter, buttons: [1, 1] });
    assert.equal(invalidButtons.ok, false);
    assert.deepEqual(moduleById(invalidButtons, shifter).buttons, [1, 3]);
    const pedal = await h.request('input', { moduleId: 'OSG-PD-0018', axes: [0, 32768, 65535] });
    assert.equal(pedal.ok, true, pedal.error);
    assert.deepEqual(moduleById(pedal, 'OSG-PD-0018').axes, [0, 32768, 65535]);
    assert.equal((await h.request('configure', { moduleId: shifter, settings: {} })).ok, false);
    await h.request('claim', { moduleId: shifter });
    assert.equal((await h.request('claim', { moduleId: shifter }, clientB)).ok, false);
    const settings = { label: 'Séquentiel 🏁', debounce: 25, inverted: true };
    const configured = await h.request('configure', { moduleId: shifter, settings });
    assert.equal(configured.ok, true, configured.error);
    assert.deepEqual(moduleById(configured, shifter).settings, settings);
    assert.equal(configured.state.needsReconnect, true, 'renaming a published USB interface requires re-enumeration');
    assert.equal((await h.request('remove', { moduleId: shifter })).ok, false);
    await h.request('release', { moduleId: shifter });

    const custom = { id: 'TEST-WHEEL-0001', name: 'Volant 🏁', kind: 'wheel', buttonCount: 32, axisCount: 6 };
    const added = await h.request('add', { module: custom });
    assert.equal(added.ok, true, added.error);
    assert.equal(added.state.needsReconnect, true);
    assert.equal(moduleById(added, custom.id).registered, false);
    assert.equal(moduleById(added, custom.id).hidInterface, null);
    assert.equal(moduleById(added, custom.id).active, false);
    assert.equal((await h.request('input', { moduleId: custom.id, buttons: [32] })).ok, false);
    assert.equal((await h.request('add', { module: custom })).ok, false);
    const reconnected = await h.request('reconnect');
    assert.equal(reconnected.ok, true, reconnected.error);
    assert.equal(reconnected.state.running, false);
    assert.equal(reconnected.state.needsReconnect, false);
    assert.equal(moduleById(reconnected, custom.id).registered, true);
    assert.equal(moduleById(reconnected, custom.id).hidInterface, 6);
    assert.deepEqual(moduleById(reconnected, shifter).settings, settings);
    await h.request('start');
    const wheelInput = await h.request('input', { moduleId: custom.id, buttons: [1, 32], axes: [0, 1, 32767, 32768, 65534, 65535] });
    assert.equal(wheelInput.ok, true, wheelInput.error);
    assert.deepEqual(moduleById(wheelInput, custom.id).buttons, [1, 32]);
    assert.deepEqual(moduleById(wheelInput, custom.id).axes, [0, 1, 32767, 32768, 65534, 65535]);
    const offline = await h.request('presence', { moduleId: custom.id, online: false });
    assert.equal(offline.ok, true);
    assert.equal(moduleById(offline, custom.id).online, false);
    assert.deepEqual(moduleById(offline, custom.id).buttons, []);
    assert.deepEqual(moduleById(offline, custom.id).axes, [0, 0, 0, 0, 0, 0]);
    assert.equal((await h.request('input', { moduleId: custom.id, buttons: [1] })).ok, false);
    await h.request('presence', { moduleId: custom.id, online: true });
    const removed = await h.request('remove', { moduleId: custom.id });
    assert.equal(removed.ok, true);
    assert.equal(moduleById(removed, custom.id), undefined);
    assert.equal(removed.state.needsReconnect, true);
    assert.equal((await h.request('add', { module: custom })).ok, false, 'published ID remains reserved until USB layout changes');
    assert.equal((await h.request('reconnect')).ok, true);
    assert.equal((await h.request('add', { module: custom })).ok, true, 'the removed ID is available after re-enumeration');
  } finally { await h.close(); }
});

test('production C inventory supports eight controllers and a zero-controller CDC-only layout', async () => {
  const h = harness();
  try {
    assert.equal((await h.connect()).ok, true);
    for (let index = 0; index < 4; index++) {
      const added = await h.request('add', { module: { id: `CUSTOM-${index}`, name: `Custom ${index}`, kind: 'panel', buttonCount: 1, axisCount: 0 } });
      assert.equal(added.ok, true, added.error);
    }
    const full = await h.request('status');
    assert.equal(full.state.modules.length, 8);
    const overflow = await h.request('add', { module: { id: 'NINTH', name: 'Ninth', kind: 'panel', buttonCount: 1, axisCount: 0 } });
    assert.equal(overflow.ok, false);
    assert.match(overflow.error, /at most 8/);
    assert.equal(overflow.state.modules.length, 8);
    const layout = await h.request('reconnect');
    assert.equal(layout.ok, true, layout.error);
    assert.deepEqual(layout.state.modules.map(module => module.hidInterface), [2, 3, 4, 5, 6, 7, 8, 9]);
    for (const module of layout.state.modules) assert.equal((await h.request('remove', { moduleId: module.id })).ok, true);
    const empty = await h.request('reconnect');
    assert.equal(empty.ok, true, empty.error);
    assert.equal(empty.state.connected, true);
    assert.equal(empty.state.running, false);
    assert.equal(empty.state.needsReconnect, false);
    assert.deepEqual(empty.state.modules, []);
    assert.equal((await h.request('start')).ok, true, 'CDC remains usable with no HID interfaces');
    assert.equal((await h.request('status')).ok, true);
    assert.equal((await h.request('stop')).state.running, false);
    const recreated = await h.request('add', { module: { id: shifter, name: 'Replacement shifter', kind: 'shifter', buttonCount: 3, axisCount: 0 } });
    assert.equal(recreated.ok, true, recreated.error);
    assert.equal(recreated.state.needsReconnect, true);
    assert.equal(moduleById(recreated, shifter).registered, false);
    assert.equal((await h.request('reconnect')).state.modules.length, 1);
  } finally { await h.close(); }
});

test('a production protocol fixture preserves inventory but ends setup and input after an unplug', async () => {
  const h = harness();
  try {
    await h.connect();
    await h.request('start');
    await h.request('claim', { moduleId: shifter });
    h.fixture.unplug();
    const unplugged = await h.request('status');
    assert.equal(unplugged.state.connected, false);
    assert.equal(unplugged.state.running, false);
    assert.deepEqual(unplugged.state.claimedModules, []);
    assert.ok(unplugged.state.modules.every(module => !module.online && !module.active));
    const connected = await h.connect();
    assert.equal(connected.ok, true, connected.error);
    assert.equal(connected.state.running, false);
    assert.equal(connected.state.modules.length, 4);
    assert.deepEqual(connected.state.claimedModules, []);
    assert.deepEqual(moduleById(connected, shifter).buttons, []);
  } finally { await h.close(); }
});
