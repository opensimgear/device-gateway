import assert from 'node:assert/strict';
import test from 'node:test';
import { encodeUF2, validateUF2, RP2040_FAMILY_ID } from '../uf2.mjs';
test('RP2040 UF2 preserves binary and addresses partial final block', () => {
  const binary = Buffer.from(Array.from({ length: 600 }, (_, i) => i % 256));
  const uf2 = encodeUF2(binary);
  assert.equal(uf2.length, 1536);
  assert.equal(uf2.readUInt32LE(512 + 12), 0x10000100);
  assert.equal(uf2.readUInt32LE(1024 + 28), RP2040_FAMILY_ID);
  assert.doesNotThrow(() => validateUF2(uf2, binary));
});
test('UF2 rejects corrupted headers, family, data and empty images', () => {
  const binary = Buffer.from([1, 2, 3]);
  const original = encodeUF2(binary);
  for (const offset of [0, 4, 8, 12, 16, 20, 24, 28, 32, 35, 508]) {
    const corrupted = Buffer.from(original);
    corrupted[offset] ^= 1;
    assert.throws(() => validateUF2(corrupted, binary));
  }
  assert.throws(() => encodeUF2(Buffer.alloc(0)));
  assert.throws(() => encodeUF2(Buffer.alloc(2 * 1024 * 1024 + 1)));
});
