// Every process and filesystem call is injected. These tests cannot rename real HID devices.
import test from 'node:test';
import assert from 'node:assert/strict';
import { EventEmitter } from 'node:events';
import { PassThrough, Writable } from 'node:stream';
import { createPicoHIDNamer } from './pico-hid-names.mjs';

const payload = () => ({ gatewaySerial: 'OSG-PICO-0123456789ABCDEF',
  modules: [{ id: 'OSG-SQ-0042', hidInterface: 2, label: 'OSG Séquentiel 🏁' }] });
const source = '/fictional/My Gateway/pico-hid-names.swift';
const binary = '/fictional/My Gateway/.build/pico-hid-names';
const cache = '/fictional/My Gateway/.build/module-cache';

function fixture(options = {}) {
  const context = { calls: [], killed: [], renamed: [], removed: [], directories: [], sourceTime: 100,
    binaryExists: false, compileFailure: false, behavior: undefined };
  function spawn(command, args, settings) {
    const call = { command, args, settings, input: '' };
    context.calls.push(call);
    const child = new EventEmitter();
    child.stdout = new PassThrough();
    child.stderr = new PassThrough();
    child.kill = signal => { context.killed.push(signal); queueMicrotask(() => child.emit('close', null, signal)); };
    child.stdin = new Writable({
      write(chunk, _encoding, callback) { call.input += chunk; callback(); },
      final(callback) {
        callback();
        queueMicrotask(async () => {
          let result;
          if (command === 'xcrun') result = context.compileFailure ? { code: 1, stderr: 'compiler rejected helper' } : { code: 0 };
          else if (context.behavior) result = await context.behavior(call, child);
          else {
            const request = JSON.parse(call.input);
            result = { code: 0, stdout: JSON.stringify({ ok: true, devices: request.modules.map(module => ({
              ...module, registryID: '18446744073709550000', product: module.label, ok: true,
              descendants: [{ class: 'IOHIDInterface', registryID: '123', product: 'old cache', ok: false, error: 'unsupported property' }],
            })) }) };
          }
          if (!result) return;
          if (result.error) { child.emit('error', result.error); return; }
          if (result.stdout) {
            const bytes = Buffer.from(result.stdout);
            if (options.fragments) for (let index = 0; index < bytes.length; index++) child.stdout.write(bytes.subarray(index, index + 1));
            else child.stdout.write(bytes);
          }
          if (result.stderr) child.stderr.write(result.stderr);
          child.stdout.end(); child.stderr.end();
          child.emit('close', result.code ?? 0, result.signal ?? null);
        });
      },
    });
    return child;
  }
  const apply = createPicoHIDNamer({ platform: 'darwin', sourcePath: source, binaryPath: binary, cachePath: cache,
    spawn, stat: async path => {
      if (path === binary && !context.binaryExists) throw new Error('ENOENT');
      return { mtimeMs: context.sourceTime, size: 1234 };
    }, mkdir: async path => { context.directories.push(path); },
    rename: async (...paths) => { context.renamed.push(paths); context.binaryExists = true; },
    rm: async path => { context.removed.push(path); }, compileTimeout: 20, helperTimeout: 20, ...options });
  return { apply, context };
}

test('unsupported systems skip both compilation and HID naming', async () => {
  const h = fixture({ platform: 'linux' });
  assert.deepEqual(await h.apply(payload()), { supported: false });
  assert.deepEqual(h.context.calls, []);
  assert.deepEqual(h.context.directories, []);
});

test('compilation is cached by source modification time and renamed atomically', async () => {
  const h = fixture();
  const first = await h.apply(payload());
  assert.equal(first.supported, true);
  assert.equal(first.ok, true);
  await h.apply(payload());
  assert.equal(h.context.calls.filter(call => call.command === 'xcrun').length, 1);
  assert.equal(h.context.calls.filter(call => call.command === binary).length, 2);
  assert.ok(h.context.directories.includes(cache));
  assert.equal(h.context.renamed.length, 1);
  assert.equal(h.context.renamed[0][1], binary);
  assert.match(h.context.renamed[0][0], /\.tmp$/);
  h.context.sourceTime++;
  await h.apply(payload());
  assert.equal(h.context.calls.filter(call => call.command === 'xcrun').length, 2);
  h.context.binaryExists = false;
  await h.apply(payload());
  assert.equal(h.context.calls.filter(call => call.command === 'xcrun').length, 3);
});

test('commands use argv and bounded JSON stdin; fragmented UTF-8 and 64-bit registry identities survive', async () => {
  const h = fixture({ fragments: true });
  const request = payload();
  request.modules[0].label = 'OSG Séquentiel 🏁 "quoted" $(literal) `text`';
  const result = await h.apply(request);
  const compile = h.context.calls[0];
  assert.equal(compile.command, 'xcrun');
  assert.deepEqual(compile.args.slice(0, 5), ['swiftc', '-O', '-module-cache-path', cache, source]);
  assert.ok(h.context.calls.every(call => call.settings.shell === false));
  assert.equal(h.context.calls[1].command, binary);
  assert.deepEqual(h.context.calls[1].args, []);
  assert.deepEqual(JSON.parse(h.context.calls[1].input), request);
  assert.equal(result.devices[0].product, request.modules[0].label);
  assert.equal(result.devices[0].registryID, '18446744073709550000');
  assert.equal(result.devices[0].descendants[0].ok, false, 'descendant cache failures remain visible without overriding main-device success');
});

test('invalid module definitions are rejected before compiling or opening a helper', async () => {
  const invalid = [
    request => { request.gatewaySerial = 'unrelated-board'; },
    request => { request.modules[0].hidInterface = null; },
    request => { request.modules[0].hidInterface = 10; },
    request => { request.modules[0].label = 'x'.repeat(81); },
    request => { request.modules[0].label = 'OSG\ninvalid'; },
    request => { request.modules[0].label = 'OSG\ud800'; },
    request => { request.modules[0].id = '../device'; },
    request => { request.modules.push({ ...request.modules[0] }); },
    request => { request.modules.push({ ...request.modules[0], id: 'OTHER-ID' }); },
    request => { request.modules = Array.from({ length: 9 }, (_, index) => ({ id: `DEVICE-${index}`, hidInterface: index + 2, label: 'OSG Device' })); },
  ];
  for (const corrupt of invalid) {
    const h = fixture();
    const request = payload();
    corrupt(request);
    await assert.rejects(h.apply(request), /Invalid Pico HID naming/);
    assert.deepEqual(h.context.calls, []);
  }
});

test('empty controller inventory is a valid naming no-op', async () => {
  const h = fixture();
  const request = payload(); request.modules = [];
  assert.deepEqual(await h.apply(request), { supported: true, ok: true, devices: [] });
});

test('a failed compilation is cleaned up and retried rather than poisoning the cache', async () => {
  const h = fixture();
  h.context.compileFailure = true;
  await assert.rejects(h.apply(payload()), /could not compile: compiler rejected helper/);
  assert.equal(h.context.renamed.length, 0);
  assert.equal(h.context.removed.length, 1);
  h.context.compileFailure = false;
  assert.equal((await h.apply(payload())).ok, true);
  assert.equal(h.context.calls.filter(call => call.command === 'xcrun').length, 2);
});

test('valid nonzero helper results retain partial device failures for the bridge', async () => {
  const h = fixture();
  h.context.behavior = call => {
    const request = JSON.parse(call.input);
    return { code: 1, stdout: JSON.stringify({ ok: false, error: 'HID service is absent',
      devices: [{ ...request.modules[0], ok: false, product: 'old product', error: 'Not found' }] }) };
  };
  const result = await h.apply(payload());
  assert.equal(result.supported, true);
  assert.equal(result.ok, false);
  assert.equal(result.error, 'HID service is absent');
  assert.equal(result.devices[0].error, 'Not found');
});

test('malformed, inconsistent, and oversized helper outputs are rejected', async () => {
  const replies = [
    { code: 0, stdout: 'not json' },
    { code: 0, stdout: JSON.stringify({ ok: true, devices: [] }) },
    { code: 1, stdout: JSON.stringify({ ok: true, devices: [] }) },
    { code: 0, stdout: JSON.stringify({ ok: false, devices: [] }) },
    { code: 0, stdout: JSON.stringify({ ok: true, devices: [{ id: 'unknown' }] }) },
  ];
  for (const reply of replies) {
    const h = fixture(); h.context.behavior = () => reply;
    await assert.rejects(h.apply(payload()), /invalid JSON|invalid result|invalid device metadata|without confirming/);
  }
  const large = fixture({ outputLimit: 100 });
  large.context.behavior = () => ({ code: 0, stdout: 'x'.repeat(101) });
  await assert.rejects(large.apply(payload()), /output exceeded/);
  assert.deepEqual(large.context.killed, ['SIGKILL']);
});

test('unresponsive helpers are killed and later requests can recover', async () => {
  const h = fixture({ helperTimeout: 5 });
  h.context.behavior = () => undefined;
  await assert.rejects(h.apply(payload()), /naming helper timed out/);
  assert.deepEqual(h.context.killed, ['SIGKILL']);
  h.context.behavior = undefined;
  assert.equal((await h.apply(payload())).ok, true);
  assert.equal(h.context.calls.filter(call => call.command === 'xcrun').length, 1);
});

test('overlapping naming requests preserve order and share one compilation', async () => {
  const h = fixture();
  const second = payload(); second.modules[0].label = 'OSG Updated';
  const results = await Promise.all([h.apply(payload()), h.apply(second)]);
  assert.equal(h.context.calls.filter(call => call.command === 'xcrun').length, 1);
  assert.deepEqual(h.context.calls.filter(call => call.command === binary).map(call => JSON.parse(call.input).modules[0].label),
    ['OSG Séquentiel 🏁', 'OSG Updated']);
  assert.equal(results[1].devices[0].product, 'OSG Updated');
});
