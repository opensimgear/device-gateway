import { mkdtempSync, mkdirSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
const directory = dirname(fileURLToPath(import.meta.url));
const temporary = mkdtempSync(resolve(tmpdir(), 'osg-pico-tests-'));
try {
  const compiler = process.env.CC || 'cc';
  const executable = resolve(temporary, 'test-gateway');
  const host = resolve(directory, 'build/host');
  mkdirSync(host, { recursive: true });
  const harness = spawnSync(compiler, ['-std=c11', '-Wall', '-Wextra', '-Werror', '-Isrc', 'src/gateway.c', 'tests/gateway_stdio.c', '-o', resolve(host, 'osg-gateway-stdio')], { cwd: directory, stdio: 'inherit' });
  if (harness.error) throw harness.error;
  if (harness.status !== 0) throw new Error('Host protocol harness build failed.');
  for (const [command, args] of [[compiler, ['-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-g', '-Isrc', 'src/gateway.c', 'src/usb_layout.c', 'tests/test_gateway.c', '-o', executable]], [executable, []], [process.execPath, ['--test', 'tests/uf2.test.mjs']]]) {
    const result = spawnSync(command, args, { cwd: directory, stdio: 'inherit' });
    if (result.error) throw result.error;
    if (result.status !== 0) throw new Error(`${command} exited with ${result.status}`);
  }
} finally { rmSync(temporary, { recursive: true, force: true }); }
