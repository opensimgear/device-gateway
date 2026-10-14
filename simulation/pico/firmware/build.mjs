import { existsSync, mkdirSync, readFileSync, writeFileSync, readdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { encodeUF2 } from './uf2.mjs';

const directory = dirname(fileURLToPath(import.meta.url));
const repository = resolve(directory, '../../..');
const research = resolve(repository, '.research');
const pins = {
  sdk: '079c6f39023649b154152db30f1d781e884879bc',
  common: '6ba8ad0c3c0a35fce2c9f0e256503ebba3957976',
  device: '53b0c19c76bbb091bd09a325642030611a41bfc4',
};
function execute(command, args, options = {}) {
  const result = spawnSync(command, args, { cwd: directory, ...options });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`${command} failed with exit status ${result.status}${result.stderr ? `: ${result.stderr}` : ''}`);
  return result;
}
function dependency(name, override, url, required) {
  const directories = { sdk: 'pico-sdk', common: 'firmware-common', device: 'device-template' };
  const path = resolve(override || resolve(research, 'deps', directories[name]));
  if (!override) {
    if (!existsSync(path)) {
      mkdirSync(dirname(path), { recursive: true });
      execute('git', ['clone', '--no-checkout', url, path], { stdio: 'inherit' });
      execute('git', ['-C', path, 'checkout', '--detach', pins[name]], { stdio: 'inherit' });
    }
    const actual = execute('git', ['-C', path, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).stdout.trim();
    if (actual !== pins[name]) throw new Error(`${path} is at ${actual}; expected ${pins[name]}. Supply an explicit source override for local development.`);
  }
  if (!existsSync(resolve(path, required))) throw new Error(`Missing ${required} in ${path}.`);
  return path;
}
const sdk = dependency('sdk', process.env.PICO_SDK_PATH, 'https://github.com/raspberrypi/pico-sdk.git', 'external/pico_sdk_import.cmake');
if (!existsSync(resolve(sdk, 'lib/tinyusb/src/tusb.h'))) {
  if (process.env.PICO_SDK_PATH) throw new Error('Initialize the TinyUSB submodule in the selected Pico SDK.');
  execute('git', ['-C', sdk, 'submodule', 'update', '--init', '--recursive', 'lib/tinyusb'], { stdio: 'inherit' });
}
const common = dependency('common', process.env.OSG_COMMON_SOURCE, 'https://github.com/opensimgear/firmware-common.git', 'src/events/events.c');
const device = dependency('device', process.env.OSG_DEVICE_SOURCE, 'https://github.com/opensimgear/device-template.git', 'firmware/src/runtime_config.c');
const cmake = process.env.OSG_CMAKE || 'cmake';
const fallbackToolchain = '/Applications/ArmGNUToolchain/14.3.rel1/arm-none-eabi/bin';
const toolchain = process.env.ARM_GNU_BIN || (existsSync(resolve(fallbackToolchain, 'arm-none-eabi-gcc')) ? fallbackToolchain : null);
const environment = { ...process.env, ...(toolchain ? { PATH: `${toolchain}:${process.env.PATH}` } : {}) };
const build = resolve(process.env.OSG_BUILD_DIR || resolve(research, 'pico/build'));
mkdirSync(build, { recursive: true });
const run = (command, args) => execute(command, args, { env: environment, stdio: 'inherit' });
run(cmake, ['-S', directory, '-B', build, `-DPICO_SDK_PATH=${sdk}`, `-DOSG_COMMON_SOURCE=${common}`, `-DOSG_DEVICE_SOURCE=${device}`, '-DPICO_BOARD=pico_w', '-DCMAKE_BUILD_TYPE=Release', '-DPICO_NO_PICOTOOL=1']);
run(cmake, ['--build', build, '--parallel', String(Math.min(8, Number(process.env.OSG_BUILD_JOBS || 4)))]);

const binary = readFileSync(resolve(build, 'osg-pico-gateway.bin'));
const storageBytes = 9 * 2 * 4096;
if (Math.ceil(binary.length / 256) * 256 > 2 * 1024 * 1024 - storageBytes) throw new Error('UF2 overlaps persistent debug journals.');
const uf2 = encodeUF2(binary);
const output = resolve(build, 'osg-pico-gateway.uf2');
writeFileSync(output, uf2);
const sha256 = createHash('sha256').update(uf2).digest('hex');
writeFileSync(`${output}.sha256`, `${sha256}  osg-pico-gateway.uf2\n`);
const commit = path => execute('git', ['-C', path, 'rev-parse', 'HEAD'], { encoding: 'utf8' }).stdout.trim();
const sizeTool = toolchain ? resolve(toolchain, 'arm-none-eabi-size') : 'arm-none-eabi-size';
const sizeOutput = execute(sizeTool, ['-A', resolve(build, 'osg-pico-gateway.elf')], { encoding: 'utf8', env: environment }).stdout;
const sections = Object.fromEntries(sizeOutput.split('\n').map(line => /^\s*(\.[\w.]+)\s+(\d+)\s+\d+\s*$/.exec(line)).filter(Boolean).map(match => [match[1], Number(match[2])]));
const stack = [];
function scan(path) {
  for (const entry of readdirSync(path, { withFileTypes: true })) {
    const item = resolve(path, entry.name);
    if (entry.isDirectory()) scan(item);
    else if (entry.name.endsWith('.su')) for (const line of readFileSync(item, 'utf8').split('\n')) {
      const match = /^(.+)\t(\d+)\t(.+)$/.exec(line);
      if (match) stack.push({ function: match[1], bytes: Number(match[2]), allocation: match[3] });
    }
  }
}
scan(build);
stack.sort((a, b) => b.bytes - a.bytes);
const ramSections = ['.ram_vector_table', '.uninitialized_data', '.data', '.tdata', '.tbss', '.tbss_space', '.bss', '.tls0', '.tls1', '.persistent_data', '.scratch_x', '.scratch_y'];
const memory = { sections, staticRamBytes: ramSections.reduce((sum, name) => sum + (sections[name] || 0), 0),
  stackReservedBytes: 24576, storageReservedBytes: storageBytes, largestStackFrames: stack.slice(0, 20) };
writeFileSync(resolve(build, 'memory-audit.json'), JSON.stringify(memory, null, 2) + '\n');
writeFileSync(resolve(build, 'firmware-manifest.json'), JSON.stringify({ firmwareVersion: '0.1.0', protocolVersion: 3,
  target: 'pico-debug', board: 'pico_w', mcu: 'rp2040', sdkCommit: commit(sdk), commonCommit: commit(common),
  deviceCommit: commit(device), maxVirtualDevices: 8, binaryBytes: binary.length, uf2Bytes: uf2.length, sha256, memory }, null, 2) + '\n');
console.log(`Built ${output} (${uf2.length} bytes; static RAM ${memory.staticRamBytes} bytes; SHA-256 ${sha256})`);
