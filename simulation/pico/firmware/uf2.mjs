/** RP2040 UF2 container. Family ID is defined by the official Pico SDK. */
export const RP2040_FAMILY_ID = 0xe48bff56;
const FLASH_BASE = 0x10000000;
const MAGIC_START0 = 0x0a324655;
const MAGIC_START1 = 0x9e5d5157;
const MAGIC_END = 0x0ab16f30;
export function encodeUF2(binary) {
  if (binary.length === 0 || binary.length > 2 * 1024 * 1024) throw new Error('Firmware does not fit Pico W flash.');
  const blocks = Math.ceil(binary.length / 256);
  const uf2 = Buffer.alloc(blocks * 512);
  for (let block = 0; block < blocks; block++) {
    const offset = block * 512;
    for (const [index, value] of [MAGIC_START0, MAGIC_START1, 0x00002000, FLASH_BASE + block * 256, 256, block, blocks, RP2040_FAMILY_ID].entries()) uf2.writeUInt32LE(value, offset + index * 4);
    binary.copy(uf2, offset + 32, block * 256, Math.min(binary.length, (block + 1) * 256));
    uf2.writeUInt32LE(MAGIC_END, offset + 508);
  }
  validateUF2(uf2, binary);
  return uf2;
}
export function validateUF2(uf2, binary) {
  const blocks = Math.ceil(binary.length / 256);
  if (uf2.length !== blocks * 512 || blocks === 0) throw new Error('Invalid UF2 length.');
  for (let block = 0; block < blocks; block++) {
    const offset = block * 512;
    const expected = [MAGIC_START0, MAGIC_START1, 0x00002000, FLASH_BASE + block * 256, 256, block, blocks, RP2040_FAMILY_ID];
    for (const [index, value] of expected.entries()) if (uf2.readUInt32LE(offset + index * 4) !== value) throw new Error(`Invalid UF2 block ${block} header word ${index}.`);
    if (uf2.readUInt32LE(offset + 508) !== MAGIC_END) throw new Error(`Invalid UF2 block ${block} end marker.`);
    const source = binary.subarray(block * 256, Math.min(binary.length, (block + 1) * 256));
    if (!uf2.subarray(offset + 32, offset + 32 + source.length).equals(source)) throw new Error(`Invalid UF2 block ${block} payload.`);
    if (uf2.subarray(offset + 32 + source.length, offset + 288).some(byte => byte !== 0)) throw new Error('Invalid UF2 payload padding.');
  }
}
