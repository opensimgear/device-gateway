import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';

const firmware = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const repository = resolve(firmware, '../../..');
const build = resolve(repository, '.research/pico/storage-coverage');
mkdirSync(build, { recursive: true });
function run(command, args, options = {}) {
  const result = spawnSync(command, args, { cwd: build, encoding: 'utf8', ...options });
  if (result.error) throw result.error;
  if (result.status !== 0) throw new Error(`${command} failed (${result.status})\n${result.stdout}\n${result.stderr}`);
  return result.stdout;
}
const source = resolve(firmware, 'src/pico_storage.c');
const includes = ['src', '../../../firmware/include', '../../../firmware/shared-cyphal/include', '../../../firmware/shared-cyphal/generated'].map(path => resolve(firmware, path));
const binary = resolve(build, 'test-storage');
run(process.env.OSG_COVERAGE_CLANG || 'clang', ['-std=c11', '-Wall', '-Wextra', '-Werror', '-fprofile-instr-generate', '-fcoverage-mapping',
  ...includes.flatMap(path => ['-I', path]), source, resolve(repository, 'firmware/src/gateway_journal.c'),
  resolve(repository, 'firmware/shared-cyphal/src/gateway_protocol.c'), resolve(firmware, 'tests/test_pico_storage.c'), '-o', binary]);
run(binary, [], { env: { ...process.env, LLVM_PROFILE_FILE: resolve(build, 'storage.profraw') } });
function llvm(name, args) {
  const override = process.env[name === 'llvm-cov' ? 'OSG_LLVM_COV' : 'OSG_LLVM_PROFDATA'];
  if (override) return run(override, args);
  return process.platform === 'darwin' ? run('xcrun', [name, ...args]) : run(name, args);
}
llvm('llvm-profdata', ['merge', '-sparse', resolve(build, 'storage.profraw'), '-o', resolve(build, 'storage.profdata')]);
const report = JSON.parse(llvm('llvm-cov', ['export', binary, '-instr-profile', resolve(build, 'storage.profdata'), source]));
writeFileSync(resolve(build, 'coverage.json'), JSON.stringify(report, null, 2) + '\n');
const measured = report.data[0].files.find(file => file.filename === source);
if (!measured) throw new Error('Pico storage coverage was not measured.');
const lines = measured.summary.lines.percent;
console.log(`${source}: ${lines.toFixed(2)}% lines`);
if (lines <= 70) throw new Error('Pico storage line coverage must exceed 70%.');
