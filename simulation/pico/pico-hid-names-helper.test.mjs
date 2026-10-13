// CLI validation never opens HID hardware: valid requests have no modules, and
// malformed requests must fail before IOHIDManager is created.
import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { test } from 'node:test';

const source = fileURLToPath(new URL('./pico-hid-names.swift', import.meta.url));

function run(command, args, chunks = []) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { stdio: ['pipe', 'pipe', 'pipe'] });
    let stdout = '', stderr = '';
    child.once('error', reject);
    child.stdin.on('error', () => {}); // Oversized input can be rejected early.
    child.stdout.on('data', data => { stdout += data; });
    child.stderr.on('data', data => { stderr += data; });
    child.once('close', code => resolve({ code, stdout, stderr }));
    const write = index => {
      if (index === chunks.length) { child.stdin.end(); return; }
      child.stdin.write(chunks[index], () => setTimeout(() => write(index + 1), 5));
    };
    write(0);
  });
}

test('Pico HID name helper validates its stdin without touching hardware', { skip: process.platform !== 'darwin' }, async t => {
  const directory = await mkdtemp(join(tmpdir(), 'osg-pico-hid-names-'));
  try {
    const binary = join(directory, 'pico-hid-names');
    const built = await run('xcrun', ['swiftc', '-module-cache-path', join(directory, 'module-cache'), source, '-o', binary]);
    assert.equal(built.code, 0, built.stderr);
    const empty = JSON.stringify({ gatewaySerial: 'OSG-PICO-E661640843565431', modules: [] });
    await t.test('valid fragmented JSON reaches EOF and succeeds', async () => {
      const result = await run(binary, [], [empty.slice(0, 17), empty.slice(17)]);
      assert.equal(result.code, 0);
      assert.deepEqual(JSON.parse(result.stdout), { ok: true, devices: [] });
    });
    await t.test('self-test runs without HID access', async () => {
      const result = await run(binary, ['--self-test']);
      assert.equal(result.code, 0);
      assert.equal(JSON.parse(result.stdout).tests, 11);
    });
    await t.test('request is bounded before JSON decoding', async () => {
      const result = await run(binary, [], [' '.repeat(8193), empty]);
      assert.equal(result.code, 2);
      assert.equal(JSON.parse(result.stdout).ok, false);
    });
    for (const [name, input] of [
      ['empty stdin', ''],
      ['malformed JSON', '{'],
      ['wrong identity', JSON.stringify({ gatewaySerial: 'OTHER-DEVICE', modules: [] })],
      ['null module list', JSON.stringify({ gatewaySerial: 'OSG-PICO-TEST', modules: null })],
      ['CDC interface', JSON.stringify({ gatewaySerial: 'OSG-PICO-TEST', modules: [{ id: 'panel', hidInterface: 1, label: 'OSG Panel' }] })],
      ['boolean interface', JSON.stringify({ gatewaySerial: 'OSG-PICO-TEST', modules: [{ id: 'panel', hidInterface: true, label: 'OSG Panel' }] })],
      ['control in name', JSON.stringify({ gatewaySerial: 'OSG-PICO-TEST', modules: [{ id: 'panel', hidInterface: 2, label: 'OSG\u0000Panel' }] })],
    ]) {
      await t.test(name, async () => {
        const result = await run(binary, [], [input]);
        assert.equal(result.code, 2);
        assert.deepEqual(JSON.parse(result.stdout), { ok: false, devices: [], error: 'Invalid Pico HID naming request.' });
      });
    }
  } finally { await rm(directory, { recursive: true, force: true }); }
});
