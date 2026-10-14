// Test-only transport. This runs the production C protocol in a host process;
// USB attachment is emulated and no physical HID controllers are created.
import { spawn } from 'node:child_process';
import { EventEmitter } from 'node:events';
import { randomUUID } from 'node:crypto';
import { fileURLToPath } from 'node:url';

const executable = fileURLToPath(new URL('../../.research/pico/host/osg-gateway-stdio', import.meta.url));
const path = '/dev/cu.usbmodemOSGTEST';
const serialNumber = 'OSG-PICO-MOCK-0123456789ABCDEF';

export function createProtocolFixture() {
  const child = spawn(executable, [], { stdio: ['pipe', 'pipe', 'pipe'] });
  let current;
  let closed = false;
  let exited = false;
  let diagnostics = '';
  let launchError;
  const launched = new Promise((resolve, reject) => {
    child.once('spawn', resolve);
    child.once('error', error => { launchError = error; reject(error); });
  });
  // A fixture may be created while the build is unavailable; open reports that error.
  void launched.catch(() => {});
  const exit = new Promise(resolve => child.once('close', () => { exited = true; resolve(); }));
  child.stderr.on('data', chunk => { diagnostics = (diagnostics + chunk).slice(-3000); });
  child.stdout.on('data', chunk => { if (current?.isOpen) current.emit('data', chunk); });
  child.stdin.on('error', error => {
    if (current?.isOpen) current.emit('error', error);
  });
  child.once('exit', (code, signal) => {
    if (current?.isOpen) {
      const handle = current;
      current = undefined;
      handle.isOpen = false;
      if (!closed) handle.emit('error', new Error(`Protocol test process exited (${signal ?? code}). ${diagnostics}`.trim()));
      handle.emit('close');
    }
  });
  function stopForDTR() {
    if (closed || exited || !child.stdin.writable) return;
    child.stdin.write(`${JSON.stringify({ id: `fixture-stop-${randomUUID()}`, command: 'stop' })}\n`, () => {});
  }
  class SerialPort extends EventEmitter {
    constructor(settings) {
      super();
      this.path = settings.path;
      this.isOpen = false;
      this.dtr = false;
      if (settings.autoOpen !== false) throw new Error('The protocol fixture requires explicit serial opening.');
    }
    open(callback) {
      void launched.then(() => {
        if (closed || exited || this.path !== path) throw new Error('Protocol fixture is unavailable. Build it with node simulation/pico/firmware/test.mjs.');
        if (current?.isOpen) throw new Error('Protocol fixture serial port is already open.');
        current = this;
        this.isOpen = true;
        callback?.();
      }).catch(error => callback?.(launchError ?? error));
    }
    set(settings, callback) {
      if (!this.isOpen) { queueMicrotask(() => callback?.(new Error('Protocol fixture port is closed.'))); return; }
      this.dtr = settings.dtr;
      if (!this.dtr) stopForDTR();
      queueMicrotask(() => callback?.());
    }
    write(bytes, callback) {
      if (!this.isOpen || closed || exited) {
        queueMicrotask(() => callback?.(new Error('Protocol fixture port is closed.')));
        return false;
      }
      return child.stdin.write(bytes, callback);
    }
    close(callback) {
      this.isOpen = false;
      this.dtr = false;
      if (current === this) current = undefined;
      queueMicrotask(() => { this.emit('close'); callback?.(); });
    }
  }
  const listPorts = async () => closed || exited ? [] : [{ path, serialNumber,
    vendorId: '1209', productId: 'f100', manufacturer: 'Protocol test fixture (no USB hardware)',
    friendlyName: 'Pico production C protocol — emulated USB attachment' }];
  SerialPort.list = listPorts;
  function unplug() {
    stopForDTR();
    const handle = current;
    current = undefined;
    if (handle) { handle.isOpen = false; handle.dtr = false; handle.emit('close'); }
  }
  async function close() {
    if (closed) return exit;
    unplug();
    closed = true;
    child.stdin.end();
    const timeout = setTimeout(() => child.kill('SIGTERM'), 500);
    timeout.unref?.();
    await exit;
    clearTimeout(timeout);
  }
  return { SerialPort, listPorts, close, unplug };
}
