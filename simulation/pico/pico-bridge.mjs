// @ts-nocheck -- Node-only serial transport; browser sources use the typed client.
import { randomUUID } from 'node:crypto';
import { StringDecoder } from 'node:string_decoder';
import { setTimeout as delay } from 'node:timers/promises';
import { applyPicoHIDNames } from './pico-hid-names.mjs';

const commands = new Set(['status', 'start', 'stop', 'input', 'register', 'reconnect', 'presence', 'configure', 'add', 'remove']);
const layoutCommands = new Set(['add', 'register', 'remove', 'reconnect', 'stop', 'disconnect', 'connect']);
const moduleKinds = new Set(['shifter', 'pedals', 'wheel', 'handbrake', 'panel']);
const serialPath = /^(?:\/dev\/(?:cu|tty)\.usbmodem[\w.-]+|\/dev\/ttyACM\d+|COM\d+)$/;
const emptyState = () => ({ backend: 'pico', protocolVersion: 1, connected: false, running: false,
  needsReconnect: false, reconnecting: false, modules: [], port: null, ports: [] });

/** Options inject a transport and short deadlines for isolated tests, never an emulated HID backend. */
export function picoGateway(options = {}) {
  const commandTimeout = options.commandTimeout ?? 3000;
  const helloTimeout = options.helloTimeout ?? 3000;
  const reconnectTimeout = options.reconnectTimeout ?? 10_000;
  const reconnectDelay = options.reconnectDelay ?? 400;
  const reconnectPoll = options.reconnectPoll ?? 250;
  const heartbeatInterval = options.heartbeatInterval ?? 2000;
  const browserTimeout = options.browserTimeout ?? 5000;
  const claimTimeout = options.claimTimeout ?? 15_000;
  // Mock transports never invoke the operating system helper unless injected.
  const applyHIDNames = options.applyHIDNames ?? (options.SerialPort ? undefined : applyPicoHIDNames);
  const namingRetry = options.namingRetry ?? 5000;
  let SerialPort = options.SerialPort;
  let serial;
  let board;
  let state = emptyState();
  let closed = false;
  let browserLastSeen = 0;
  let browserRequests = 0;
  let buffer = '';
  let decoder = new StringDecoder('utf8');
  let sequence = Promise.resolve();
  let heartbeat;
  let hidNaming;
  let namingTask;
  let namingSignature;
  let namingLastAttempt = 0;
  let namingEpoch = 0;
  const pending = new Map();
  const claims = new Map();

  function refreshHIDNames(force = false) {
    if (!applyHIDNames || closed || !state.connected || state.reconnecting) return;
    if (force) { namingEpoch++; namingSignature = undefined; }
    if (namingTask) return;
    const payload = { gatewaySerial: state.gatewaySerial,
      modules: state.modules.filter(module => module.registered).map(module => ({
        id: module.id, hidInterface: module.hidInterface, label: module.settings.label,
      })) };
    const signature = JSON.stringify(payload);
    if (signature === namingSignature && (hidNaming?.applied || hidNaming?.supported === false || Date.now() - namingLastAttempt < namingRetry)) return;
    namingSignature = signature;
    namingLastAttempt = Date.now();
    const epoch = namingEpoch;
    hidNaming = { supported: process.platform === 'darwin', applied: false };
    // Compilation and registry naming never block CDC commands or heartbeats.
    namingTask = Promise.resolve().then(() => applyHIDNames(payload)).then(result => {
      if (epoch !== namingEpoch || closed || !state.connected) return;
      hidNaming = { supported: result.supported !== false, applied: result.ok === true,
        ...(result.ok === false ? { error: result.error ?? 'macOS could not apply all HID controller names.' } : {}) };
    }).catch(error => {
      if (epoch === namingEpoch && !closed && state.connected) hidNaming = {
        supported: process.platform === 'darwin', applied: false,
        error: error instanceof Error ? error.message : 'macOS HID naming failed.',
      };
    }).finally(() => { namingTask = undefined; refreshHIDNames(); });
  }

  async function serialClass() {
    SerialPort ??= (await import('serialport')).SerialPort;
    return SerialPort;
  }
  async function ports() {
    const list = options.listPorts ? await options.listPorts() : await (await serialClass()).list();
    state.ports = list.filter(item => typeof item.path === 'string' && serialPath.test(item.path)).map(item => {
      const port = { path: item.path };
      for (const key of ['manufacturer', 'serialNumber', 'vendorId', 'productId', 'friendlyName']) {
        if (typeof item[key] === 'string') port[key] = item[key].slice(0, 256);
      }
      return port;
    });
    return state.ports;
  }
  function neutralize() {
    state = { ...state, connected: false, running: false, reconnecting: false, port: null,
      modules: state.modules.map(module => ({ ...module, online: false, active: false,
        buttons: [], axes: Array(module.axisCount ?? 0).fill(0) })) };
  }
  function failPending(message) {
    for (const request of pending.values()) { clearTimeout(request.timer); request.reject(new Error(message)); }
    pending.clear();
    claims.clear();
  }
  async function closePort(message = 'Pico disconnected.') {
    namingEpoch++;
    namingSignature = undefined;
    hidNaming = undefined;
    const handle = serial;
    serial = undefined;
    buffer = '';
    decoder = new StringDecoder('utf8');
    failPending(message);
    neutralize();
    if (!handle) return;
    // Dropping DTR also neutralizes reports on firmware when a command cannot finish.
    await new Promise(resolve => {
      if (!handle.isOpen) return resolve();
      let closing = false;
      const finish = () => {
        if (closing) return;
        closing = true;
        clearTimeout(setTimeoutHandle);
        const timeout = setTimeout(resolve, 500);
        try { handle.close(() => { clearTimeout(timeout); resolve(); }); }
        catch { clearTimeout(timeout); resolve(); }
      };
      const setTimeoutHandle = setTimeout(finish, 500);
      try { handle.set({ dtr: false, rts: false }, finish); }
      catch { finish(); }
    });
  }
  function transportFailure(handle, message) {
    if (serial !== handle) return;
    void closePort(message);
  }
  function acceptState(next) {
    const printable = value => typeof value === 'string' && value.trim().length > 0 &&
      Array.from(value).length <= 80 && !/[\u0000-\u001f\u007f]/.test(value);
    const validModule = module => module && typeof module.id === 'string' && /^[\w-]{1,40}$/.test(module.id) && printable(module.name) && moduleKinds.has(module.kind) &&
      ['online', 'registered', 'active'].every(key => typeof module[key] === 'boolean') &&
      Number.isInteger(module.buttonCount) && module.buttonCount >= 0 && module.buttonCount <= 32 &&
      Number.isInteger(module.axisCount) && module.axisCount >= 0 && module.axisCount <= 6 &&
      module.buttonCount + module.axisCount > 0 &&
      Array.isArray(module.buttons) && module.buttons.length <= module.buttonCount &&
      module.buttons.every(button => Number.isInteger(button) && button >= 1 && button <= module.buttonCount) &&
      new Set(module.buttons).size === module.buttons.length &&
      Array.isArray(module.axes) && module.axes.length === module.axisCount &&
      module.axes.every(axis => Number.isInteger(axis) && axis >= 0 && axis <= 65535) &&
      module.serialNumber === `OSG-SIM-${module.id}` && module.usbSerialNumber === next.gatewaySerial &&
      (module.hidInterface === null || (Number.isInteger(module.hidInterface) && module.hidInterface >= 2 && module.hidInterface <= 9)) &&
      module.registered === (module.hidInterface !== null) &&
      (!module.active || (module.registered && next.running && !next.reconnecting)) &&
      module.settings && printable(module.settings.label) && Number.isInteger(module.settings.debounce) &&
      module.settings.debounce >= 0 && module.settings.debounce <= 100 && typeof module.settings.inverted === 'boolean';
    if (!next || next.backend !== 'pico' || next.protocolVersion !== 1 ||
      typeof next.gatewaySerial !== 'string' || !/^OSG-PICO-[\w-]{4,64}$/.test(next.gatewaySerial) ||
      ['running', 'needsReconnect', 'reconnecting'].some(key => typeof next[key] !== 'boolean') ||
      !Array.isArray(next.modules) || next.modules.length > 8 || next.modules.some(module => !validModule(module)) ||
      new Set(next.modules.map(module => module.id)).size !== next.modules.length ||
      new Set(next.modules.filter(module => module.registered).map(module => module.hidInterface)).size !== next.modules.filter(module => module.registered).length) {
      throw new Error('This serial device is not a supported OpenSimGear Pico gateway (protocol 1).');
    }
    if (board?.gatewaySerial && next.gatewaySerial !== board.gatewaySerial) throw new Error('A different Pico gateway appeared on the serial port. Connect it explicitly.');
    state = { ...next, ports: state.ports, connected: true, port: serial?.path ?? board?.path ?? null };
  }
  function receive(handle, chunk) {
    if (serial !== handle) return;
    buffer += decoder.write(chunk);
    if (Buffer.byteLength(buffer) > 65_536) {
      transportFailure(handle, 'Pico sent an oversized serial response.');
      return;
    }
    let newline;
    while ((newline = buffer.indexOf('\n')) >= 0) {
      const line = buffer.slice(0, newline).trim();
      buffer = buffer.slice(newline + 1);
      if (!line) continue;
      let response;
      try { response = JSON.parse(line); }
      catch { transportFailure(handle, 'Pico sent malformed serial data. Reconnect the board.'); return; }
      if (!response || typeof response.id !== 'string') continue;
      const request = pending.get(response.id);
      if (!request) continue;
      clearTimeout(request.timer);
      pending.delete(response.id);
      request.resolve(response);
    }
  }
  async function send(request, timeout = commandTimeout) {
    const handle = serial;
    if (!handle?.isOpen) throw new Error('Connect the flashed Pico gateway first.');
    const id = randomUUID();
    // Browser identities and setup leases stay local to the bridge.
    const { clientId: _clientId, ...payload } = request;
    const wire = `${JSON.stringify({ ...payload, id })}\n`;
    if (Buffer.byteLength(wire) > 4095) throw new Error('Pico request exceeds the firmware’s 4095-byte serial frame limit.');
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        transportFailure(handle, 'Pico did not respond. Its connection and setup sessions were closed; reconnect the board.');
      }, timeout);
      pending.set(id, { resolve, reject, timer });
      try {
        handle.write(wire, error => {
          if (error) transportFailure(handle, `Pico serial write failed: ${error.message}`);
        });
      } catch (error) { transportFailure(handle, `Pico serial write failed: ${error.message}`); }
    });
  }
  async function exchange(request, timeout) {
    const result = await send(request, timeout);
    try {
      if (typeof result.ok !== 'boolean' || (!result.ok && typeof result.error !== 'string')) throw new Error('Pico sent an invalid gateway response.');
      if (result.state) acceptState(result.state);
      else if (result.ok) throw new Error('Pico response did not include its gateway state.');
    } catch (error) {
      await closePort(error.message);
      throw error;
    }
    return { ...result, state };
  }
  async function openPort(info, expectedSerial) {
    if (closed) throw new Error('Cockpit server has closed.');
    const Port = await serialClass();
    if (closed) throw new Error('Cockpit server has closed.');
    const handle = new Port({ path: info.path, baudRate: 115200, autoOpen: false });
    serial = handle;
    buffer = '';
    decoder = new StringDecoder('utf8');
    handle.on('data', chunk => receive(handle, chunk));
    handle.on('error', error => transportFailure(handle, `Pico serial connection failed: ${error.message}`));
    handle.on('close', () => transportFailure(handle, 'Pico was unplugged. Connect it again to resume.'));
    try {
      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error('Opening the Pico serial port timed out.')), helloTimeout);
        handle.open(error => {
          clearTimeout(timeout);
          if (serial !== handle || closed) {
            if (handle.isOpen) handle.close(() => {});
            reject(new Error('Pico connection closed while opening.'));
          } else error ? reject(error) : resolve();
        });
      });
      if (serial !== handle || closed) throw new Error('Pico connection closed while opening.');
      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error('Setting Pico serial control lines timed out.')), helloTimeout);
        handle.set({ dtr: true, rts: false }, error => { clearTimeout(timeout); error ? reject(error) : resolve(); });
      });
      // Never trust a listed CDC device until it identifies itself with this protocol.
      board = expectedSerial ? { ...info, gatewaySerial: expectedSerial } : undefined;
      const hello = await exchange({ command: 'hello' }, helloTimeout);
      if (!hello.ok) throw new Error(hello.error ?? 'Pico refused the gateway handshake.');
      board = { ...info, gatewaySerial: state.gatewaySerial };
      return hello;
    } catch (error) {
      await closePort(error.message);
      if (handle.isOpen) handle.close(() => {});
      throw error;
    }
  }
  async function disconnect() {
    if (serial?.isOpen) {
      try { await exchange({ command: 'stop' }); } catch { /* Closing DTR is the fallback. */ }
    }
    await closePort();
    return { ok: true, state };
  }
  async function reconnect() {
    const identity = { ...board };
    const ack = await exchange({ command: 'reconnect' });
    if (!ack.ok) return ack;
    if (!state.reconnecting || state.running || state.modules.some(module => module.active)) {
      await closePort();
      throw new Error('Pico did not acknowledge stopped controller reports before USB re-enumeration.');
    }
    claims.clear();
    await delay(reconnectDelay);
    await closePort('Pico is applying its USB controller layout.');
    const deadline = Date.now() + reconnectTimeout;
    let lastError;
    while (Date.now() < deadline && !closed) {
      const available = await ports();
      // Match the USB serial descriptor, not the old path: macOS can rename CDC ports.
      const candidate = available.find(item => identity.serialNumber
        ? item.serialNumber === identity.serialNumber && (!identity.vendorId || item.vendorId === identity.vendorId)
        : item.path === identity.path);
      if (candidate) {
        try {
          await openPort(candidate, identity.gatewaySerial);
          return await exchange({ command: 'status' });
        } catch (error) { lastError = error; }
      }
      await delay(reconnectPoll);
    }
    throw new Error(`Pico did not return after changing its USB layout. Connect the board again.${lastError ? ` ${lastError.message}` : ''}`);
  }
  function expireClaims(clientId) {
    const now = Date.now();
    for (const [id, lease] of claims) {
      if (now - lease.lastSeen > claimTimeout) claims.delete(id);
      else if (lease.clientId === clientId) lease.lastSeen = now;
    }
  }
  function decorate(result, clientId) {
    return { ...result, state: { ...state,
      ...(hidNaming ? { hidNaming } : {}),
      claimedModules: [...claims].filter(([, lease]) => lease.clientId === clientId).map(([id]) => id) } };
  }
  async function handle(request) {
    if (closed) throw new Error('Cockpit server has closed.');
    if (!request || typeof request !== 'object' || Array.isArray(request)) throw new Error('Invalid Pico gateway request.');
    const { clientId, command, moduleId } = request;
    if (typeof clientId !== 'string' || !/^[\w-]{16,80}$/.test(clientId)) throw new Error('Missing simulator client identity. Reload Cockpit.');
    expireClaims(clientId);
    if (layoutCommands.has(command) && claims.size) throw new Error('End active setup sessions before changing the gateway layout.');
    let result;
    if (command === 'ports') {
      await ports();
      result = { ok: true, state };
    } else if (command === 'connect') {
      const available = await ports();
      const selected = available.find(item => item.path === request.port);
      if (!selected) throw new Error('Select an available USB serial port for the flashed Pico gateway.');
      await disconnect();
      await openPort(selected);
      const stopped = await exchange({ command: 'stop' });
      if (!stopped.ok) { await closePort(); throw new Error(stopped.error ?? 'Pico could not stop its controller reports.'); }
      result = await exchange({ command: 'status' });
    } else if (command === 'disconnect') {
      result = await disconnect();
    } else if (command === 'status' && !serial) {
      await ports();
      result = { ok: true, state };
    } else if (command === 'claim') {
      const owner = claims.get(moduleId);
      if (owner && owner.clientId !== clientId) throw new Error('Another Cockpit tab owns this module’s setup session.');
      result = await exchange({ command: 'status' });
      const module = state.modules.find(module => module.id === moduleId);
      if (!module?.online) throw new Error('Reconnect this module before starting setup.');
      claims.set(moduleId, { clientId, lastSeen: Date.now() });
    } else if (command === 'release') {
      if (claims.get(moduleId)?.clientId === clientId) claims.delete(moduleId);
      result = serial ? await exchange({ command: 'status' }) : { ok: true, state };
    } else {
      if (!commands.has(command)) throw new Error('Unknown Pico gateway command.');
      if (command === 'configure' && claims.get(moduleId)?.clientId !== clientId) throw new Error('Start a setup session before saving this module.');
      if (command === 'presence' && !request.online && claims.has(moduleId) && claims.get(moduleId).clientId !== clientId) {
        throw new Error('Another Cockpit tab owns this module’s setup session.');
      }
      result = command === 'reconnect' ? await reconnect() : await exchange(request);
      if (result.ok && command === 'presence' && !request.online) claims.delete(moduleId);
    }
    refreshHIDNames(command === 'connect' || command === 'reconnect');
    return decorate(result, clientId);
  }
  function queue(work) {
    const operation = sequence.then(work);
    sequence = operation.catch(() => {});
    return operation;
  }
  function startHeartbeat() {
    if (heartbeat) return;
    heartbeat = setInterval(() => {
      void queue(async () => {
        if (!serial || closed || browserRequests) return;
        if (Date.now() - browserLastSeen > browserTimeout) {
          await closePort('Cockpit stopped checking the Pico. Connect it again to resume.');
          return;
        }
        expireClaims();
        try { await exchange({ command: 'status' }); refreshHIDNames(); } catch { /* Failure already closes the transport. */ }
      });
    }, heartbeatInterval);
    heartbeat.unref?.();
  }
  function close() {
    if (closed) return;
    closed = true;
    clearInterval(heartbeat);
    if (serial?.isOpen) serial.write(`${JSON.stringify({ id: randomUUID(), command: 'stop' })}\n`, () => {});
    void closePort('Cockpit server stopped.');
  }
  function install(server) {
    startHeartbeat();
    server.httpServer?.once('close', close);
    server.middlewares.use(options.endpoint ?? '/api/pico-gateway', async (req, res) => {
      res.setHeader('Content-Type', 'application/json');
      res.setHeader('Cache-Control', 'no-store');
      const host = req.headers.host ?? '';
      const loopback = ['127.0.0.1', '::1', '::ffff:127.0.0.1'].includes(req.socket.remoteAddress);
      if (!loopback || !/^(localhost|127\.0\.0\.1|\[::1\])(?::\d+)?$/.test(host) ||
        (req.headers.origin && req.headers.origin !== `http://${host}`) || req.headers[options.header ?? 'x-osg-simulator'] !== '1') {
        res.statusCode = 403;
        res.end(JSON.stringify({ ok: false, error: 'The Pico gateway accepts requests from this local Cockpit only.' }));
        return;
      }
      if (req.method !== 'POST' || req.url !== '/' || !req.headers['content-type']?.startsWith('application/json')) {
        res.statusCode = 405; res.end(JSON.stringify({ ok: false, error: 'Use POST application/json.' })); return;
      }
      let request;
      try {
        let body = '';
        for await (const chunk of req) {
          body += chunk;
          if (Buffer.byteLength(body) > 8192) throw new Error('Pico gateway request is too large.');
        }
        request = JSON.parse(body);
        browserLastSeen = Date.now();
        browserRequests++;
        try {
          const result = await queue(() => handle(request));
          res.end(JSON.stringify(result));
        } finally {
          browserRequests--;
          browserLastSeen = Date.now();
        }
      } catch (error) {
        res.statusCode = 400;
        res.end(JSON.stringify({ ok: false, error: error instanceof Error ? error.message : 'Pico gateway request failed.',
          state: decorate({}, request?.clientId).state }));
      }
    });
  }
  return { name: 'opensimgear-pico-gateway', configureServer: install, configurePreviewServer: install, closeBundle: close };
}
