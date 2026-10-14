import { mkdtempSync, mkdirSync, rmSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
const directory = dirname(fileURLToPath(import.meta.url));
const repository = resolve(directory, '../../..');
const artifacts = resolve(repository, '.research/pico/host');
mkdirSync(artifacts, { recursive: true });
const temporary = mkdtempSync(resolve(artifacts, 'tests-'));
try {
  const compiler = process.env.CC || 'cc';
  const executable = resolve(temporary, 'test-gateway');
  const host = artifacts;
  mkdirSync(host, { recursive: true });
  const harness = spawnSync(compiler, ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Isrc', 'src/gateway.c', 'tests/gateway_stdio.c', '-o', resolve(host, 'osg-gateway-stdio')], { cwd: directory, stdio: 'inherit' });
  if (harness.error) throw harness.error;
  if (harness.status !== 0) throw new Error('Host protocol harness build failed.');
  for (const [command, args] of [[compiler, ['-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g', '-Isrc', 'src/gateway.c', 'src/usb_layout.c', 'tests/test_gateway.c', '-o', executable]], [executable, []], [process.execPath, ['--test', 'tests/uf2.test.mjs']]]) {
    const result = spawnSync(command, args, { cwd: directory, stdio: 'inherit' });
    if (result.error) throw result.error;
    if (result.status !== 0) throw new Error(`${command} exited with ${result.status}`);
  }
  const storage = resolve(temporary, 'test-storage');
  for (const [command, args] of [[compiler, ['-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g',
    '-Isrc', `-I${resolve(repository, 'firmware/include')}`, `-I${resolve(repository, 'firmware/shared-cyphal/include')}`,
    `-I${resolve(repository, 'firmware/shared-cyphal/generated')}`, 'src/pico_storage.c',
    resolve(repository, 'firmware/src/gateway_journal.c'), resolve(repository, 'firmware/shared-cyphal/src/gateway_protocol.c'),
    'tests/test_pico_storage.c', '-o', storage]], [storage, []], [process.execPath, ['tests/storage-coverage.mjs']], [process.execPath, ['tests/debug-test.mjs']]]) {
    const result = spawnSync(command, args, { cwd: directory, stdio: 'inherit' });
    if (result.error) throw result.error;
    if (result.status !== 0) throw new Error(`${command} exited with ${result.status}`);
  }
} finally { rmSync(temporary, { recursive: true, force: true }); }
