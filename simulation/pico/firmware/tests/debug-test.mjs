import { existsSync, mkdirSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const firmware = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repository = resolve(firmware, '../../..');
const common = process.env.OSG_COMMON_SOURCE || resolve(repository, '.research/deps/firmware-common');
const device = process.env.OSG_DEVICE_SOURCE || resolve(repository, '.research/deps/device-template');
const build = resolve(repository, '.research/pico/tests');
mkdirSync(build, { recursive: true });
if (!existsSync(resolve(common, 'include/opensimgear/events.h')) || !existsSync(resolve(device, 'firmware/src/runtime_config.c'))) {
  throw new Error('Initialize pinned dependencies with firmware/build.mjs, or set OSG_COMMON_SOURCE and OSG_DEVICE_SOURCE.');
}
const includes = ['src', '../../../firmware/include', '../../../firmware/shared-publication/include', '../../../firmware/shared-cyphal/include', '../../../firmware/shared-cyphal/generated'].map(path => resolve(firmware, path));
includes.push(resolve(common, 'include'), resolve(device, 'firmware/include'));
const sources = [
  'src/debug_gateway.c', 'src/virtual_device.c',
  ...['gateway.c', 'gateway_registry.c', 'gateway_layout.c', 'gateway_cockpit.c', 'gateway_journal.c', 'gateway_publication.c'].map(path => `../../../firmware/src/${path}`),
  '../../../firmware/shared-publication/src/publication.c',
  '../../../firmware/shared-cyphal/src/gateway_protocol.c',
].map(path => resolve(firmware, path));
sources.push(...['runtime_config.c', 'device_actions.c'].map(path => resolve(device, 'firmware/src', path)),
  ...['inputs/profile.c', 'inputs/inputs.c', 'inputs/button.c', 'actions/actions.c', 'events/events.c'].map(path => resolve(common, 'src', path)));
const compiler = process.env.CC || 'clang';
function run(command, args, options = {}) {
  const result = spawnSync(command, args, { cwd: build, encoding: 'utf8', ...options });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`${command} failed (${result.status})\n${result.stdout}\n${result.stderr}`);
  return result.stdout;
}
const flags = ['-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-UNDEBUG', ...includes.flatMap(path => ['-I', path])];
const test = resolve(build, 'test-debug');
run(compiler, [...flags, '-fsanitize=address,undefined', ...sources, resolve(firmware, 'tests/test_debug.c'), '-o', test]);
process.stdout.write(run(test, []));
const harness = resolve(repository, '.research/pico/host/osg-debug-stdio');
mkdirSync(dirname(harness), { recursive: true });
run(compiler, [...flags, ...sources, resolve(firmware, 'tests/debug_stdio.c'), '-o', harness]);

// Changed portable debug logic has an explicit >70% line gate. Shared production
// dependencies retain their own coverage suites; this gate measures the new path.
const coverage = resolve(build, 'test-debug-coverage');
run(process.env.OSG_COVERAGE_CLANG || 'clang', [...flags, '-fprofile-instr-generate', '-fcoverage-mapping', ...sources, resolve(firmware, 'tests/test_debug.c'), '-o', coverage]);
run(coverage, [], { env: { ...process.env, LLVM_PROFILE_FILE: resolve(build, 'debug.profraw') } });
const llvm = name => {
  const override = process.env[name === 'llvm-cov' ? 'OSG_LLVM_COV' : 'OSG_LLVM_PROFDATA'];
  return override ? [override, []] : process.platform === 'darwin' ? ['xcrun', [name]] : [name, []];
};
let [command, prefix] = llvm('llvm-profdata');
run(command, [...prefix, 'merge', '-sparse', resolve(build, 'debug.profraw'), '-o', resolve(build, 'debug.profdata')]);
[command, prefix] = llvm('llvm-cov');
const report = JSON.parse(run(command, [...prefix, 'export', coverage, '-instr-profile', resolve(build, 'debug.profdata'), ...sources.slice(0, 2)]));
let measured = 0;
for (const file of report.data[0].files) {
  if (!sources.slice(0, 2).includes(file.filename)) continue;
  measured++;
  const lines = file.summary.lines.percent;
  console.log(`${file.filename}: ${lines.toFixed(2)}% lines`);
  if (lines <= 70) throw new Error('Pico debug firmware line coverage must exceed 70%.');
}
if (measured !== 2) throw new Error('Both new Pico debug sources must be measured.');
console.log(`Built asynchronous debug protocol harness: ${harness}`);
