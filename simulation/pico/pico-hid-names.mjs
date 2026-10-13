// @ts-nocheck -- macOS-only Node helper; no hardware access occurs on import.
import { spawn as spawnProcess } from 'node:child_process';
import { stat as fileStat, mkdir as makeDirectory, rename as renameFile, rm as removeFile } from 'node:fs/promises';
import { randomUUID } from 'node:crypto';
import { dirname } from 'node:path';
import { StringDecoder } from 'node:string_decoder';
import { fileURLToPath } from 'node:url';

const sourcePath = fileURLToPath(new URL('./pico-hid-names.swift', import.meta.url));
const binaryPath = fileURLToPath(new URL('./.build/pico-hid-names', import.meta.url));
const cachePath = fileURLToPath(new URL('./.build/pico-hid-names-module-cache', import.meta.url));

function encodeRequest(payload) {
  if (!payload || typeof payload !== 'object' || Array.isArray(payload) ||
    typeof payload.gatewaySerial !== 'string' || !/^OSG-PICO-[\w-]{4,64}$/.test(payload.gatewaySerial) ||
    !Array.isArray(payload.modules) || payload.modules.length > 8) throw new Error('Invalid Pico HID naming request.');
  const ids = new Set();
  const interfaces = new Set();
  const modules = payload.modules.map(module => {
    if (!module || typeof module.id !== 'string' || !/^[\w-]{1,40}$/.test(module.id) || ids.has(module.id) ||
      !Number.isInteger(module.hidInterface) || module.hidInterface < 2 || module.hidInterface > 9 || interfaces.has(module.hidInterface) ||
      typeof module.label !== 'string' || !module.label.trim() || Array.from(module.label).length > 80 ||
      /[\p{Cc}\p{Surrogate}]/u.test(module.label)) throw new Error('Invalid Pico HID naming module. Use unique interfaces 2–9 and labels of 1–80 printable Unicode characters.');
    ids.add(module.id);
    interfaces.add(module.hidInterface);
    return { id: module.id, hidInterface: module.hidInterface, label: module.label };
  });
  const request = { gatewaySerial: payload.gatewaySerial, modules };
  const wire = `${JSON.stringify(request)}\n`;
  if (Buffer.byteLength(wire) > 8192) throw new Error('Pico HID naming request exceeds 8192 bytes.');
  return { request, wire };
}

/** Injectable process/filesystem dependencies keep unit tests away from real HID devices. */
export function createPicoHIDNamer(options = {}) {
  const platform = options.platform ?? process.platform;
  const spawn = options.spawn ?? spawnProcess;
  const stat = options.stat ?? fileStat;
  const mkdir = options.mkdir ?? makeDirectory;
  const rename = options.rename ?? renameFile;
  const rm = options.rm ?? removeFile;
  const source = options.sourcePath ?? sourcePath;
  const binary = options.binaryPath ?? binaryPath;
  const moduleCache = options.cachePath ?? cachePath;
  const compileTimeout = options.compileTimeout ?? 45_000;
  const helperTimeout = options.helperTimeout ?? 5000;
  const outputLimit = options.outputLimit ?? 65_536;
  let compiledStamp;
  let sequence = Promise.resolve();

  function run(command, args, stdin, timeout) {
    return new Promise((resolve, reject) => {
      let child;
      let settled = false;
      let stdout = '';
      let stderr = '';
      let outputBytes = 0;
      const stdoutDecoder = new StringDecoder('utf8');
      const stderrDecoder = new StringDecoder('utf8');
      let timer;
      const fail = error => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        child?.kill('SIGKILL');
        reject(error);
      };
      try { child = spawn(command, args, { stdio: ['pipe', 'pipe', 'pipe'], shell: false, windowsHide: true }); }
      catch (error) { fail(error); return; }
      timer = setTimeout(() => fail(new Error(`Pico HID ${command === 'xcrun' ? 'helper compilation' : 'naming helper'} timed out.`)), timeout);
      const collect = (chunk, isError) => {
        outputBytes += Buffer.byteLength(chunk);
        if (outputBytes > outputLimit) { fail(new Error('Pico HID helper output exceeded its limit.')); return; }
        if (isError) stderr += stderrDecoder.write(chunk);
        else stdout += stdoutDecoder.write(chunk);
      };
      child.stdout.on('data', chunk => collect(chunk, false));
      child.stderr.on('data', chunk => collect(chunk, true));
      child.once('error', fail);
      child.stdin.once('error', fail);
      child.once('close', (code, signal) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        stdout += stdoutDecoder.end();
        stderr += stderrDecoder.end();
        resolve({ code, signal, stdout, stderr });
      });
      try { child.stdin.end(stdin ?? ''); }
      catch (error) { fail(error); }
    });
  }
  async function compile() {
    const metadata = await stat(source);
    const stamp = `${metadata.mtimeMs}:${metadata.size}`;
    if (compiledStamp === stamp) {
      try { await stat(binary); return; } catch { compiledStamp = undefined; }
    }
    await Promise.all([mkdir(dirname(binary), { recursive: true }), mkdir(moduleCache, { recursive: true })]);
    const temporary = `${binary}.${randomUUID()}.tmp`;
    try {
      const result = await run('xcrun', ['swiftc', '-O', '-module-cache-path', moduleCache, source, '-o', temporary], undefined, compileTimeout);
      if (result.code !== 0) throw new Error(`Pico HID helper could not compile: ${result.stderr.trim().slice(-2000) || `exit ${result.signal ?? result.code}`}`);
      await rename(temporary, binary);
      // If the source changed during compilation, the next request recompiles it.
      const latest = await stat(source);
      compiledStamp = `${latest.mtimeMs}:${latest.size}` === stamp ? stamp : undefined;
    } finally { await rm(temporary, { force: true }); }
  }
  async function apply({ request, wire }) {
    await compile();
    const result = await run(binary, [], wire, helperTimeout);
    let response;
    try { response = JSON.parse(result.stdout.trim()); }
    catch { throw new Error(`Pico HID naming helper returned invalid JSON${result.stderr.trim() ? `: ${result.stderr.trim().slice(-1000)}` : '.'}`); }
    if (!response || typeof response !== 'object' || Array.isArray(response) || typeof response.ok !== 'boolean' ||
      !Array.isArray(response.devices) || response.devices.length > 8 ||
      (response.error !== undefined && typeof response.error !== 'string') ||
      (!response.ok && typeof response.error !== 'string')) throw new Error('Pico HID naming helper returned an invalid result.');
    const requested = new Map(request.modules.map(module => [module.id, module]));
    const seen = new Set();
    for (const device of response.devices) {
      const module = requested.get(device?.id);
      if (!module || seen.has(device.id) || device.hidInterface !== module.hidInterface || device.label !== module.label ||
        typeof device.ok !== 'boolean' || (device.product !== undefined && typeof device.product !== 'string') ||
        (device.registryID !== undefined && typeof device.registryID !== 'string') ||
        (device.ok && (device.product !== module.label || typeof device.registryID !== 'string')) ||
        (device.error !== undefined && typeof device.error !== 'string') ||
        (device.descendants !== undefined && (!Array.isArray(device.descendants) || device.descendants.length > 64 ||
          device.descendants.some(child => !child || typeof child.class !== 'string' || typeof child.registryID !== 'string' ||
            typeof child.product !== 'string' || typeof child.ok !== 'boolean' ||
            (child.error !== undefined && typeof child.error !== 'string'))))) {
        throw new Error('Pico HID naming helper returned invalid device metadata.');
      }
      seen.add(device.id);
    }
    if ((result.code !== 0 && response.ok) || (response.ok && response.devices.some(device => !device.ok)) ||
      (response.ok && response.devices.length !== request.modules.length)) throw new Error('Pico HID naming helper exited without confirming every requested controller.');
    return { ...response, supported: true };
  }
  return async function applyPicoHIDNames(payload) {
    if (platform !== 'darwin') return { supported: false };
    const encoded = encodeRequest(payload);
    // Preserve rename order even when browser status and setup saves overlap.
    const operation = sequence.then(() => apply(encoded));
    sequence = operation.catch(() => {});
    return operation;
  };
}

export const applyPicoHIDNames = createPicoHIDNamer();
