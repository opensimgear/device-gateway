// Browser verification only: production C protocol with emulated serial attachment.
// This process never opens USB hardware or creates real HID controllers.
import { createServer } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { existsSync } from 'node:fs';
import { picoGateway } from './pico-bridge.mjs';
import { createProtocolFixture } from './pico-protocol-fixture.mjs';

const repository = fileURLToPath(new URL('../../', import.meta.url));
const root = process.env.COCKPIT_SOURCE ?? resolve(repository, '.e2e/deps/cockpit');
if (!existsSync(resolve(root, 'index.html'))) throw new Error('Set COCKPIT_SOURCE to an initialized Cockpit checkout.');
const fixture = createProtocolFixture();
let attached = process.env.OSG_UI_USB_DISCONNECTED !== '1';
process.on('SIGUSR1', () => {
  attached = !attached;
  if (!attached) fixture.unplug();
  console.log(`Fixture USB ${attached ? 'attached' : 'detached'}`);
});
const server = await createServer({
  root,
  configFile: false,
  plugins: [svelte(), picoGateway({
    endpoint: '/api/gateway/', header: 'x-osg-client',
    SerialPort: fixture.SerialPort,
    listPorts: async () => !attached ? [] : (await fixture.listPorts()).map(port => ({ ...port, vendorId: 'cafe', productId: '4010' })),
    reconnectDelay: 25,
    reconnectPoll: 25,
    reconnectTimeout: 2000,
  })],
  server: { host: '127.0.0.1', port: 5174, strictPort: true },
});
let closing = false;
async function close() {
  if (closing) return;
  closing = true;
  await server.close();
  await fixture.close();
}
for (const signal of ['SIGINT', 'SIGTERM']) process.once(signal, () => {
  void close().finally(() => process.exit(0));
});
try {
  await server.listen();
  console.log('NO USB HARDWARE: Pico browser test fixture at http://127.0.0.1:5174/ uses production C protocol over emulated serial.');
} catch (error) {
  await close();
  throw error;
}
