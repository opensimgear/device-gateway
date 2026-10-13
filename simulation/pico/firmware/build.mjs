import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { encodeUF2 } from './uf2.mjs';

const directory = dirname(fileURLToPath(import.meta.url));
const repository = resolve(directory, '../../..');
const sdk = process.env.PICO_SDK_PATH || resolve(repository, '.e2e/deps/pico-sdk');
if (!existsSync(resolve(sdk, 'external/pico_sdk_import.cmake')) || !existsSync(resolve(sdk, 'lib/tinyusb/src/tusb.h'))) {
  throw new Error('Pico SDK and its TinyUSB submodule are missing. Follow simulation/pico/README.md setup instructions, or set PICO_SDK_PATH.');
}
const cmake = process.env.OSG_CMAKE || 'cmake';
const fallbackToolchain = '/Applications/ArmGNUToolchain/14.3.rel1/arm-none-eabi/bin';
const toolchain = process.env.ARM_GNU_BIN || (existsSync(resolve(fallbackToolchain, 'arm-none-eabi-gcc')) ? fallbackToolchain : null);
const environment = { ...process.env, ...(toolchain ? { PATH: `${toolchain}:${process.env.PATH}` } : {}) };
const build = resolve(directory, 'build');
mkdirSync(build, { recursive: true });
function run(command, args) {
  const result = spawnSync(command, args, { cwd: directory, env: environment, stdio: 'inherit' });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`${command} failed with exit status ${result.status}`);
}
run(cmake, ['-S', directory, '-B', build, `-DPICO_SDK_PATH=${sdk}`, '-DPICO_BOARD=pico_w', '-DCMAKE_BUILD_TYPE=Release', '-DPICO_NO_PICOTOOL=1']);
run(cmake, ['--build', build, '--parallel', String(Math.min(8, Number(process.env.OSG_BUILD_JOBS || 4)))]);

const binary = readFileSync(resolve(build, 'osg-pico-gateway.bin'));
const uf2 = encodeUF2(binary);
const output = resolve(build, 'osg-pico-gateway.uf2');
writeFileSync(output, uf2);
const sha256 = createHash('sha256').update(uf2).digest('hex');
writeFileSync(`${output}.sha256`, `${sha256}  osg-pico-gateway.uf2\n`);
const sdkCommit = spawnSync('git', ['rev-parse', 'HEAD'], { cwd: sdk, encoding: 'utf8' }).stdout.trim();
writeFileSync(resolve(build, 'firmware-manifest.json'), JSON.stringify({ firmwareVersion: '0.1.1', protocolVersion: 1, board: 'pico_w', mcu: 'rp2040', sdkCommit, binaryBytes: binary.length, uf2Bytes: uf2.length, sha256 }, null, 2) + '\n');
console.log(`Built ${output} (${uf2.length} bytes, SHA-256 ${sha256})`);
