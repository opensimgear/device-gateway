// These tests use an in-memory serial transport. They never open or flash hardware.
import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { Readable } from 'node:stream';
import { setTimeout as delay } from 'node:timers/promises';
import { picoGateway } from './pico-bridge.mjs';

const clientA = 'test-client-a-0123456789';
const clientB = 'test-client-b-0123456789';
const path = '/dev/cu.usbmodemOSG1';
const gatewaySerial = 'OSG-PICO-0123456789ABCDEF';
const moduleId = 'OSG-SQ-0042';
const baseState = () => ({ backend: 'pico', protocolVersion: 1, gatewaySerial,
  running: false, needsReconnect: false, reconnecting: false,
  modules: [{ id: moduleId, online: true, registered: true, active: false,
    name: 'Shifter', kind: 'shifter', serialNumber: `OSG-SIM-${moduleId}`, usbSerialNumber: gatewaySerial, hidInterface: 2,
    buttonCount: 3, axisCount: 0, buttons: [], axes: [],
    settings: { label: 'Shifter', debounce: 10, inverted: false } }] });

function harness(options = {}) {
  const context = { opened: [], writes: [], handles: [],
    available: [{ path, serialNumber: gatewaySerial, vendorId: '1209', productId: 'f100' }], state: baseState() };
  class MockPort extends EventEmitter {
    constructor(settings) { super(); assert.equal(settings.autoOpen, false); this.path = settings.path; this.isOpen = false; context.handles.push(this); }
    open(callback) { context.opened.push(this.path); this.isOpen = true; queueMicrotask(() => callback()); }
    set(settings, callback) { this.dtr = settings.dtr; queueMicrotask(() => callback()); }
    close(callback) { this.isOpen = false; queueMicrotask(() => { this.emit('close'); callback?.(); }); }
    write(line, callback) {
      const request = JSON.parse(line);
      context.writes.push(request);
      queueMicrotask(() => {
        callback?.();
        if (!this.isOpen) return;
        const result = options.respond ? options.respond(request, context, this) : reply(request, context);
        if (result) this.respond({ id: request.id, ...result });
      });
    }
    respond(value) {
      const encoded = `${JSON.stringify(value)}\n`;
      // Real CDC streams can split a JSON reply at any byte and include unrelated IDs.
      if (options.utf8Fragments) {
        const bytes = Buffer.from(encoded);
        for (let index = 0; index < bytes.length; index++) this.emit('data', bytes.subarray(index, index + 1));
      } else if (options.fragments) {
        this.emit('data', Buffer.from('{"id":"old-id","ok":true}\n' + encoded.slice(0, 7)));
        this.emit('data', Buffer.from(encoded.slice(7, -2)));
        this.emit('data', Buffer.from(encoded.slice(-2)));
      } else this.emit('data', Buffer.from(encoded));
    }
  }
  function reply(request, ctx) {
    if (request.command === 'start') {
      ctx.state.running = true;
      ctx.state.modules.forEach(module => { module.active = true; });
    } else if (request.command === 'stop') {
      ctx.state.running = false;
      ctx.state.modules.forEach(module => { module.active = false; });
    }
    return { ok: true, state: structuredClone(ctx.state) };
  }
  context.reply = reply;
  const server = new EventEmitter();
  let middleware;
  const plugin = picoGateway({ SerialPort: MockPort, listPorts: async () => context.available,
    commandTimeout: 40, helloTimeout: 40, reconnectTimeout: 150, reconnectDelay: 5, reconnectPoll: 5,
    heartbeatInterval: 1000, ...options.timings,
    ...(options.applyHIDNames ? { applyHIDNames: options.applyHIDNames } : {}) });
  plugin.configureServer({ httpServer: server, middlewares: { use(route, handler) {
    assert.equal(route, '/api/pico-gateway'); middleware = handler;
  } } });
  async function request(command, extra = {}, clientId = clientA, override = {}) {
    const body = JSON.stringify({ command, clientId, ...extra });
    const req = Readable.from([Buffer.from(body)]);
    Object.assign(req, { method: 'POST', url: '/', socket: { remoteAddress: '127.0.0.1' },
      ...override, headers: { host: '127.0.0.1:5173', origin: 'http://127.0.0.1:5173',
        'content-type': 'application/json', 'x-osg-simulator': '1', ...override.headers } });
    let response;
    const res = { statusCode: 200, setHeader() {}, end(value) { response = JSON.parse(value); } };
    await middleware(req, res);
    assert.ok(response);
    return { status: res.statusCode, ...response };
  }
  const connect = () => request('connect', { port: path });
  const close = () => server.emit('close');
  return { context, request, connect, close };
}

test('enumerating ports never opens hardware; only a listed CDC path may be connected', async () => {
  const h = harness();
  try {
    h.context.available.push({ path: '/tmp/arbitrary-file' }, { path: '/dev/cu.usbserial-UART' });
    const initial = await h.request('status');
    assert.equal(initial.ok, true);
    assert.equal(initial.state.connected, false);
    assert.deepEqual(initial.state.modules, []);
    assert.deepEqual(initial.state.ports.map(port => port.path), [path]);
    assert.deepEqual(h.context.opened, []);
    assert.equal((await h.request('ports')).state.ports[0].serialNumber, gatewaySerial);
    assert.equal((await h.request('connect', { port: '/tmp/arbitrary-file' })).ok, false);
    assert.equal((await h.request('start')).ok, false);
    assert.deepEqual(h.context.opened, []);
  } finally { h.close(); }
});

test('loopback, Host, Origin, header, method, and request bounds protect the serial bridge', async () => {
  const h = harness();
  try {
    for (const override of [{ headers: { origin: 'https://unrelated.example' } },
      { headers: { host: 'remote.example' } }, { headers: { 'x-osg-simulator': undefined } },
      { socket: { remoteAddress: '192.168.1.5' } }]) {
      assert.equal((await h.request('status', {}, clientA, override)).status, 403);
    }
    assert.equal((await h.request('status', {}, clientA, { method: 'GET' })).status, 405);
    assert.equal((await h.request('status', {}, clientA, { url: '/extra' })).status, 405);
    assert.match((await h.request('add', { padding: 'x'.repeat(9000) })).error, /too large/);
    assert.equal((await h.request('status', {}, 'bad')).status, 400);
    assert.deepEqual(h.context.opened, []);
  } finally { h.close(); }
});

test('fragmented and unrelated NDJSON responses match the pending request; connect requires explicit Start', async () => {
  const h = harness({ fragments: true });
  h.context.state.running = true;
  h.context.state.modules[0].active = true;
  try {
    const result = await h.connect();
    assert.equal(result.ok, true);
    assert.equal(result.state.connected, true);
    assert.equal(result.state.port, path);
    assert.equal(result.state.running, false);
    assert.deepEqual(h.context.writes.map(request => request.command), ['hello', 'stop', 'status']);
    assert.ok(h.context.writes.every(request => !('clientId' in request)));
    assert.equal((await h.request('start')).state.running, true);
    assert.equal((await h.request('disconnect')).state.connected, false);
    assert.equal(h.context.handles[0].dtr, false);
  } finally { h.close(); }
});

test('a serial device with the wrong backend or protocol is closed without sending controller commands', async () => {
  for (const changed of [{ backend: 'unrelated' }, { protocolVersion: 2 }, { gatewaySerial: 'another-device' }]) {
    const h = harness();
    Object.assign(h.context.state, changed);
    try {
      const result = await h.connect();
      assert.equal(result.ok, false);
      assert.match(result.error, /not a supported/);
      assert.equal(result.state.connected, false);
      assert.deepEqual(h.context.writes.map(request => request.command), ['hello']);
      assert.equal(h.context.handles[0].isOpen, false);
    } finally { h.close(); }
  }
});

test('UTF-8 module labels remain intact when serial packets split inside a character', async () => {
  const h = harness({ utf8Fragments: true });
  h.context.state.modules[0].settings.label = 'Pédales 🏁';
  try {
    const result = await h.connect();
    assert.equal(result.state.modules[0].settings.label, 'Pédales 🏁');
  } finally { h.close(); }
});

test('timeouts and unplugging close the transport, neutralize last-known modules, and invalidate claims', async () => {
  const h = harness({ respond(request, ctx) { if (request.command === 'input') return; return ctx.reply(request, ctx); } });
  try {
    await h.connect();
    await h.request('start');
    await h.request('claim', { moduleId });
    const result = await h.request('input', { moduleId, buttons: [1] });
    assert.equal(result.ok, false);
    assert.match(result.error, /did not respond/);
    assert.equal(result.state.connected, false);
    assert.equal(result.state.running, false);
    assert.equal(result.state.modules[0].online, false);
    assert.equal(result.state.modules[0].active, false);
    assert.deepEqual(result.state.modules[0].buttons, []);
    assert.deepEqual(result.state.claimedModules, []);
    const count = h.context.opened.length;
    await h.request('status');
    assert.equal(h.context.opened.length, count, 'status never silently reconnects after failure');
    await h.connect();
    h.context.handles.at(-1).isOpen = false;
    h.context.handles.at(-1).emit('close');
    const unplugged = await h.request('status');
    assert.equal(unplugged.state.connected, false);
    assert.equal(unplugged.state.modules[0].online, false);
  } finally { h.close(); }
});

test('setup leases protect configuration and dynamic inventory across Cockpit tabs', async () => {
  const h = harness();
  try {
    await h.connect();
    assert.match((await h.request('configure', { moduleId, settings: {} })).error, /setup session/);
    assert.deepEqual((await h.request('claim', { moduleId })).state.claimedModules, [moduleId]);
    assert.match((await h.request('claim', { moduleId }, clientB)).error, /Another Cockpit tab/);
    for (const command of ['add', 'register', 'remove', 'reconnect', 'stop', 'disconnect', 'connect']) {
      const denied = await h.request(command, { moduleId, port: path });
      assert.match(denied.error, /End active setup sessions/);
      assert.deepEqual(denied.state.claimedModules, [moduleId], 'rejected mutation preserves the owner’s lease');
    }
    assert.equal((await h.request('configure', { moduleId, settings: {} }, clientB)).ok, false);
    await h.request('release', { moduleId }, clientB);
    assert.equal((await h.request('claim', { moduleId }, clientB)).ok, false);
    await h.request('release', { moduleId });
    assert.equal((await h.request('add', { module: { id: 'OSG-TEST-0001' } })).ok, true);
    assert.equal((await h.request('remove', { moduleId: 'OSG-TEST-0001' })).ok, true);
    assert.ok(h.context.writes.some(request => request.command === 'add'));
    assert.ok(h.context.writes.some(request => request.command === 'remove'));
  } finally { h.close(); }
});

test('USB layout reconnect finds a renamed CDC port by serial and validates the same board', async () => {
  const nextPath = '/dev/cu.usbmodemOSG9';
  const h = harness({ respond(request, ctx) {
    if (request.command === 'reconnect') {
      const ack = structuredClone(ctx.state);
      ack.reconnecting = true;
      ack.running = false;
      ack.modules.forEach(module => { module.active = false; });
      ctx.available[0].path = nextPath;
      ctx.state.modules.push({ ...ctx.state.modules[0], id: 'OSG-NEW-0001', serialNumber: 'OSG-SIM-OSG-NEW-0001', hidInterface: 3 });
      ctx.state.running = false;
      ctx.state.modules.forEach(module => { module.active = false; });
      ctx.state.reconnecting = false;
      ctx.state.needsReconnect = false;
      return { ok: true, state: ack };
    }
    return ctx.reply(request, ctx);
  } });
  try {
    await h.connect();
    await h.request('start');
    const result = await h.request('reconnect');
    assert.equal(result.ok, true);
    assert.equal(result.state.connected, true);
    assert.equal(result.state.port, nextPath);
    assert.equal(result.state.running, false);
    assert.equal(result.state.reconnecting, false);
    assert.equal(result.state.modules.length, 2);
    assert.deepEqual(h.context.opened, [path, nextPath]);
    const last = h.context.writes.slice(-2).map(request => request.command);
    assert.deepEqual(last, ['hello', 'status']);
  } finally { h.close(); }
});

test('expired setup leases can be claimed by another tab; an offline module cannot be claimed', async () => {
  const h = harness({ timings: { claimTimeout: 10 } });
  try {
    await h.connect();
    await h.request('claim', { moduleId });
    await delay(20);
    assert.deepEqual((await h.request('claim', { moduleId }, clientB)).state.claimedModules, [moduleId]);
    await h.request('release', { moduleId }, clientB);
    h.context.state.modules[0].online = false;
    assert.match((await h.request('claim', { moduleId })).error, /Reconnect this module/);
  } finally { h.close(); }
});

test('the firmware byte limit is checked before writing, while preserving a valid session', async () => {
  const h = harness();
  try {
    await h.connect();
    await h.request('claim', { moduleId });
    const writes = h.context.writes.length;
    const result = await h.request('configure', { moduleId, settings: { label: 'é'.repeat(2100) } });
    assert.equal(result.ok, false);
    assert.match(result.error, /4095-byte serial frame limit/);
    assert.equal(result.state.connected, true);
    assert.deepEqual(result.state.claimedModules, [moduleId]);
    assert.equal(h.context.writes.length, writes);
    assert.equal((await h.request('status')).ok, true);
  } finally { h.close(); }
});

test('malformed or oversized board responses invalidate the connected transport', async () => {
  for (const line of ['not-json\n', 'x'.repeat(65_537)]) {
    const h = harness();
    try {
      await h.connect();
      await h.request('claim', { moduleId });
      h.context.handles[0].emit('data', Buffer.from(line));
      const result = await h.request('status');
      assert.equal(result.state.connected, false);
      assert.deepEqual(result.state.claimedModules, []);
      assert.equal(h.context.handles[0].isOpen, false);
    } finally { h.close(); }
  }
});

test('USB reconnect refuses a different gateway even when the port metadata appears unchanged', async () => {
  const h = harness({ timings: { reconnectTimeout: 30 }, respond(request, ctx) {
    if (request.command === 'reconnect') {
      const ack = structuredClone(ctx.state);
      ack.reconnecting = true;
      ack.running = false;
      ctx.state.gatewaySerial = 'OSG-PICO-DIFFERENTBOARD';
      ctx.state.modules.forEach(module => { module.usbSerialNumber = ctx.state.gatewaySerial; });
      return { ok: true, state: ack };
    }
    return ctx.reply(request, ctx);
  } });
  try {
    await h.connect();
    const result = await h.request('reconnect');
    assert.equal(result.ok, false);
    assert.match(result.error, /different Pico/);
    assert.equal(result.state.connected, false);
    assert.ok(h.context.handles.every(handle => !handle.isOpen));
    assert.deepEqual(h.context.writes.slice(4).filter(request => request.command !== 'hello'), []);
  } finally { h.close(); }
});

test('malformed module envelopes cannot become connected HID snapshots', async () => {
  const invalid = [
    ctx => { ctx.state.modules[0].online = 'yes'; },
    ctx => { ctx.state.modules[0].active = true; },
    ctx => { ctx.state.modules[0].kind = 'keyboard'; },
    ctx => { ctx.state.modules[0].buttons = [0]; },
    ctx => { ctx.state.modules[0].axes = [65536]; },
    ctx => { ctx.state.modules[0].settings = null; },
    ctx => { ctx.state.modules[0].hidInterface = 20; },
    ctx => { ctx.state.modules[0].usbSerialNumber = 'different'; },
    ctx => { ctx.state.modules.push(structuredClone(ctx.state.modules[0])); },
  ];
  for (const corrupt of invalid) {
    const h = harness();
    corrupt(h.context);
    try {
      const result = await h.connect();
      assert.equal(result.ok, false);
      assert.equal(result.state.connected, false);
      assert.deepEqual(result.state.modules, []);
      assert.equal(h.context.handles[0].isOpen, false);
      assert.deepEqual(h.context.writes.map(request => request.command), ['hello']);
    } finally { h.close(); }
  }
});

test('a disappearing browser closes serial instead of keeping the firmware watchdog alive indefinitely', async () => {
  const h = harness({ timings: { heartbeatInterval: 5, browserTimeout: 15 } });
  try {
    await h.connect();
    await h.request('start');
    await delay(50);
    assert.equal(h.context.handles[0].isOpen, false);
    assert.equal(h.context.handles[0].dtr, false);
    const opened = h.context.opened.length;
    const status = await h.request('status');
    assert.equal(status.state.connected, false);
    assert.equal(status.state.running, false);
    assert.equal(h.context.opened.length, opened);
  } finally { h.close(); }
});

test('server shutdown closes the board and rejects requests without reconnecting', async () => {
  const h = harness();
  await h.connect();
  await h.request('start');
  h.close();
  await delay(5);
  const status = await h.request('status');
  assert.equal(status.ok, false);
  assert.match(status.error, /server has closed/);
  assert.equal(status.state.connected, false);
  assert.ok(h.context.writes.some(request => request.command === 'stop'));
  assert.equal(h.context.handles[0].isOpen, false);
});

test('slow native HID naming does not block serial commands and receives authoritative interface labels', async () => {
  let finish;
  const calls = [];
  const h = harness({ applyHIDNames: payload => {
    calls.push(payload);
    return new Promise(resolve => { finish = resolve; });
  } });
  try {
    const connected = await h.connect();
    assert.equal(connected.ok, true);
    assert.equal(connected.state.hidNaming.applied, false);
    assert.deepEqual(calls, [{ gatewaySerial, modules: [{ id: moduleId, hidInterface: 2, label: 'Shifter' }] }]);
    assert.equal((await h.request('start')).state.running, true, 'HID naming cannot hold the CDC queue');
    finish({ supported: true, ok: true, devices: [] });
    await delay(0);
    assert.equal((await h.request('status')).state.hidNaming.applied, true);
    assert.equal(calls.length, 1, 'unchanged labels do not invoke the native helper again');
    h.context.state.modules[0].settings.label = 'OSG Shifter';
    await h.request('status');
    assert.equal(calls.length, 2);
    assert.equal(calls[1].modules[0].label, 'OSG Shifter');
    await h.request('disconnect');
    finish({ supported: true, ok: true, devices: [] });
    await delay(0);
    assert.equal((await h.request('status')).state.hidNaming, undefined, 'late naming results cannot revive disconnected state');
  } finally { h.close(); }
});

test('native naming failure is visible and retried without disconnecting the gateway', async () => {
  let attempts = 0;
  const h = harness({ timings: { namingRetry: 5 }, applyHIDNames: async () => {
    if (++attempts === 1) throw new Error('HID registry access denied');
    return { supported: true, ok: true, devices: [] };
  } });
  try {
    await h.connect();
    await delay(0);
    const failed = await h.request('status');
    assert.equal(failed.state.connected, true);
    assert.match(failed.state.hidNaming.error, /registry access denied/);
    assert.equal((await h.request('start')).state.running, true);
    await delay(10);
    await h.request('status');
    await delay(0);
    const recovered = await h.request('status');
    assert.equal(recovered.state.hidNaming.applied, true);
    assert.equal(recovered.state.hidNaming.error, undefined);
    assert.equal(attempts, 2);
  } finally { h.close(); }
});
