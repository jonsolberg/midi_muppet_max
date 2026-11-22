// Part of MIDI Muppet Max – Copyright (C) 2026 Jon Solberg – GPL v3 or later, see LICENSE.
// End-to-end test: the real editor page in headless Chrome, with navigator.serial replaced by a
// fake port wired to the firmware running in simavr (optionally behind a real Arduino bootloader).
const {spawn} = require('child_process');
const LOG = process.env.SIMLOG ? require('fs').createWriteStream(process.env.SIMLOG) : null;
const T0 = Date.now();
const log = s => LOG && LOG.write(((Date.now() - T0) / 1000).toFixed(3) + ' ' + s + '\n');
const fs = require('fs');
const puppeteer = require('puppeteer-core');

const [,, appElf, mcu, bootHex, tag, mhzArg] = process.argv;
const MHZ = mhzArg || '16';
const path = require('path');
const ROOT = path.join(__dirname, '..');
const EDITOR = 'file://' + path.join(ROOT, 'editor', 'index.html');
const SHOTS = path.join(ROOT, 'build', 'shots');
fs.mkdirSync(SHOTS, {recursive: true});

// ---------------------------------------------------------------- harness bridge
const args = [appElf, mcu]; if (bootHex) args.push(bootHex);
const sim = spawn(path.join(__dirname, 'harness'), args);
let outBuf = '', waiting = null, lineQ = [];
sim.stdout.on('data', d => {
  outBuf += d; let i;
  while ((i = outBuf.indexOf('\n')) >= 0) { const l = outBuf.slice(0, i); outBuf = outBuf.slice(i + 1); lineQ.push(l); if (waiting) waiting(); }
});
sim.stderr.on('data', () => {});
const nextLine = () => new Promise(r => { if (lineQ.length) r(lineQ.shift()); else waiting = () => { waiting = null; r(lineQ.shift()); }; });
async function simCmd(c, until) {           // send a command, collect output lines until 'until' matches
  sim.stdin.write(c + '\n');
  if (!until) return [];
  const out = [];
  for (;;) { const l = await nextLine(); out.push(l); if (until(l)) return out; }
}

let page = null, pending = [], busy = false, txLog = [], portOpen = false;
async function pump() {
  if (busy) return; busy = true;
  try {
    const cmds = pending; pending = [];
    for (const c of cmds) { log('> ' + c); await simCmd(c); }
    const out = await simCmd('run 10', l => l === 'DONE');
    for (const l of out) if (l.startsWith('TX ')) {
      const h = l.slice(3); txLog.push(h); log('< ' + h);
      if (page && portOpen) await page.evaluate(h => window.__simRx(h), h).catch(() => {});
    }
  } finally { busy = false; }
}
const timer = setInterval(pump, 10);
const sleep = ms => new Promise(r => setTimeout(r, ms));
const simQuery = async (c, prefix) => { while (busy) await sleep(2); busy = true; try { const o = await simCmd(c, l => l.startsWith(prefix)); return o[o.length - 1].slice(prefix.length); } finally { busy = false; } };

// ---------------------------------------------------------------- fake Web Serial
const FAKE_SERIAL = (firefox, dead) => {
  // firefox: like Firefox, end the incoming stream when the line glitches (port opened, Arduino
  // restarted) and offer a new port.readable, which the editor has to pick up.
  let rx = null;
  const port = {
    readable: null, writable: null, _dtrLow: false,
    _newStream() {
      if (dead) {   // like Firefox with a CH340 on Windows: every stream is already finished
        Object.defineProperty(this, 'readable', {configurable: true, get: () => this._open ? new ReadableStream({start(c) { c.close(); }}) : null});
        return;
      }
      this.readable = new ReadableStream({start(c) { rx = c; }, cancel() { rx = null; }});
    },
    _glitch() { if (!firefox || !this.readable) return; setTimeout(() => { try { rx && rx.close(); } catch (e) {} this._newStream(); }, 5); },
    async open(o) {
      if (this._open) throw new Error('already open');
      this._open = true;
      this._newStream();
      this.writable = new WritableStream({write(chunk) { window.__simWrite(Array.from(chunk)); }});
      await window.__simOpen(true, o.baudRate);  // opening a port pulses DTR: the Arduino restarts
      this._glitch();
    },
    async close() {
      try { rx && rx.close(); } catch (e) {}
      this._open = false;
      if (!dead) this.readable = null;
      rx = null; this.writable = null;
      await window.__simOpen(false);
    },
    async setSignals(s) {
      if (s.dataTerminalReady === false) this._dtrLow = true;
      else if (s.dataTerminalReady && this._dtrLow) { this._dtrLow = false; await window.__simReset(); this._glitch(); }
    },
    getInfo() { return {usbVendorId: 0x1a86, usbProductId: 0x7523}; },
  };
  window.__simRx = h => { if (rx) try { rx.enqueue(new Uint8Array(h.match(/../g).map(x => parseInt(x, 16)))); } catch (e) {} };
  Object.defineProperty(navigator, 'serial', {value: {requestPort: async () => port, getPorts: async () => [port], addEventListener() {}}});
};

// ---------------------------------------------------------------- fake WebUSB CH340 (Android)
// Like Chrome on Android: Web Serial only lists Bluetooth devices, so the editor must use WebUSB.
// The fake decodes the CH340 vendor requests: baud rate registers and the DTR line.
const FAKE_USB = () => {
  Object.defineProperty(navigator, 'serial', {value: {requestPort: async () => { throw new Error('Web Serial used on Android'); }, addEventListener() {}}});
  let rxQ = [], rxWait = null, isOpen = false, dtr = false, reg = {}, baud = 0;
  const dev = {
    vendorId: 0x1a86, productId: 0x7523, configuration: null, opened: false,
    async open() { this.opened = true; isOpen = true; },
    async close() { this.opened = false; isOpen = false; dtr = false; if (rxWait) { const w = rxWait; rxWait = null; w.reject(new DOMException('closed', 'NetworkError')); } await window.__simOpen(false); },
    async selectConfiguration() { this.configuration = {interfaces: [{interfaceNumber: 0, alternate: {interfaceClass: 0xff, endpoints: [
      {endpointNumber: 2, direction: 'in', type: 'bulk', packetSize: 32},
      {endpointNumber: 2, direction: 'out', type: 'bulk', packetSize: 32},
      {endpointNumber: 1, direction: 'in', type: 'interrupt', packetSize: 8}]}}]}; },
    async claimInterface() {},
    async controlTransferIn(p, n) { return {status: 'ok', data: new DataView(new Uint8Array(n).buffer)}; },
    async controlTransferOut(p) {
      if (p.requestType !== 'vendor') throw new Error('CH340 got a non-vendor request');
      if (p.request === 0x9a) {
        reg[p.value] = p.index;
        if (p.value === 0x0f2c && reg[0x1312] !== undefined) {   // both baud registers written
          const v1 = reg[0x1312], f = 0x10000 - ((v1 & 0xff00) | p.index), d = v1 & 7;
          baud = Math.round(1532620800 / (f * Math.pow(8, 3 - d)));
          await window.__simBaud(baud);
        }
      }
      if (p.request === 0xa4) {
        const on = !(p.value & 0x20);
        if (on && !dtr) await window.__simOpen(true, baud);    // DTR goes active: the Arduino restarts
        dtr = on;
      }
      return {status: 'ok'};
    },
    transferIn(ep, n) {
      if (!isOpen) return Promise.reject(new DOMException('not open', 'InvalidStateError'));
      if (rxQ.length) { const b = rxQ.splice(0, n); return Promise.resolve({status: 'ok', data: new DataView(new Uint8Array(b).buffer)}); }
      return new Promise((resolve, reject) => { rxWait = {resolve: () => resolve(dev.transferIn(ep, n)), reject}; });
    },
    async transferOut(ep, data) { window.__simWrite(Array.from(new Uint8Array(data.buffer || data, data.byteOffset || 0, data.byteLength))); return {status: 'ok', bytesWritten: data.byteLength}; },
  };
  window.__simRx = h => { if (!isOpen) return; rxQ.push(...h.match(/../g).map(x => parseInt(x, 16))); if (rxWait) { const w = rxWait; rxWait = null; w.resolve(); } };
  Object.defineProperty(navigator, 'usb', {value: {requestDevice: async o => { window.__usbFilters = o.filters; return dev; }, addEventListener() {}}});
};

// ---------------------------------------------------------------- tests
let failures = 0;
const check = (name, ok, extra = '') => { if (!ok) failures++; console.log((ok ? 'PASS ' : 'FAIL ') + name + (ok ? '' : ' ' + extra)); };
const text = sel => page.$eval(sel, e => e.textContent);
async function waitText(sel, re, ms) {
  const end = Date.now() + ms;
  while (Date.now() < end) { const t = await text(sel).catch(() => ''); if (re.test(t)) return t; await sleep(100); }
  return await text(sel).catch(() => '');
}

(async () => {
  const browser = await puppeteer.launch({executablePath: process.env.CHROME || '/usr/bin/google-chrome', headless: true, args: ['--no-sandbox', '--allow-file-access-from-files']});
  page = await browser.newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  page.on('dialog', d => d.accept());
  await page.exposeFunction('__simWrite', bytes => { pending.push('sendhex ' + Buffer.from(bytes).toString('hex')); });
  await page.exposeFunction('__simReset', () => { pending.push('reset'); });
  await page.exposeFunction('__simOpen', (open, baud) => { portOpen = open; if (open) { pending.push('baud ' + baud); pending.push('reset'); } });
  await page.exposeFunction('__simBaud', b => { pending.push('baud ' + b); });
  if (process.env.USB_CH340) {
    await page.setUserAgent('Mozilla/5.0 (Linux; Android 16; Pixel 9) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/148.0.0.0 Mobile Safari/537.36');
    await page.evaluateOnNewDocument(FAKE_USB);
  } else await page.evaluateOnNewDocument(FAKE_SERIAL, !!process.env.FIREFOX_LIKE, !!process.env.FIREFOX_DEAD);
  await page.setViewport({width: 1280, height: 1000});
  await page.goto(EDITOR);
  await page.screenshot({path: `${SHOTS}/${tag}-1-start.png`, fullPage: true});
  check('page loads without script errors', errors.length === 0, errors.join('; '));

  if (process.env.FIREFOX_DEAD) {     // only check that the editor gives up with a clear message
    const t0 = Date.now();
    await page.click('#btnConnect');
    const m = await waitText('#pedalMsg', /CH340|did not answer/, 20000);
    check('dead stream: clear message about Firefox and CH340: ' + m, /CH340/.test(m));
    check('dead stream: gives up within 6 s (' + ((Date.now() - t0) / 1000).toFixed(1) + ' s)', Date.now() - t0 < 6000);
    await browser.close(); clearInterval(timer); sim.stdin.write('quit\n');
    console.log(`\n${tag}: ${failures} failures`); process.exit(failures ? 1 : 0);
  }

  // 1. install the firmware through the bootloader
  if (bootHex) {
    await page.select('#board', MHZ);
    await page.click('#btnFlash');
    const m = await waitText('#flashMsg', /✓|failed|No bootloader|not an/, 240000);
    check('firmware installed through the bootloader: ' + m, /✓/.test(m), m);
    const hexText = fs.readFileSync(path.join(ROOT, 'firmware', `midi_muppet_max_config-${mcu}-${MHZ}mhz.hex`), 'utf8');
    const img = Buffer.alloc(32768, 0xff); let top = 0;
    for (const l of hexText.split('\n')) if (l.startsWith(':') && l.slice(7, 9) === '00') {
      const b = Buffer.from(l.slice(1).trim(), 'hex'); const a = b.readUInt16BE(1); b.copy(img, a, 4, 4 + b[0]); top = Math.max(top, a + b[0]); }
    const flash = await simQuery(`flash 0 ${top}`, 'FL ');
    check(`flash memory holds the new firmware (${top} bytes)`, flash === img.slice(0, top).toString('hex').toUpperCase());
  }

  // 2. connect
  await page.click('#btnConnect');
  const st = await waitText('#statusText', /^Connected|Not connected/, 20000);
  check('connects to the pedal', st === 'Connected', st + ' / ' + await text('#pedalMsg'));
  if (process.env.USB_CH340) check('went through WebUSB (CH340 driver)', /usb: ch340 ready/.test(await text('#log')));
  const info = await text('#pedalInfo');
  check('shows firmware, chip and clock: ' + info, info.includes(mcu === 'atmega168' ? 'ATmega168' : 'ATmega328P') && info.includes(MHZ + ' MHz'));
  const legend = await page.$$eval('#legend li', l => l.map(x => x.textContent));
  check('default setup read from the pedal', /PC 1 · long: PC 5/.test(legend[0]), legend[0]);
  await page.screenshot({path: `${SHOTS}/${tag}-2-connected.png`, fullPage: true});

  // 3. physical button press shows up live
  pending.push('pin 3 0'); await sleep(400);
  const live = await page.$eval('#legend li[data-b="2"]', e => e.className);
  check('pressing switch 3 on the pedal highlights it', live.includes('live'), live);
  check('pressing switch 3 selects it in the editor', (await text('#btnTitle')) === 'Switch 3');
  pending.push('pin 3 1'); await sleep(300);

  // 4. edit: switch 1 short = HX tap tempo, long = RC-500 next memory; switch 3 long = CC toggle 80
  // each press has two lists: the pedal, then that pedal's commands
  await page.click('#legend li[data-b="0"]');
  await page.select('.press[data-slot="0"] select[data-dev]', 'hx');
  await page.select('.press[data-slot="0"] select[data-choice]', 'hx-tap');
  await page.select('.press[data-slot="1"] select[data-dev]', 'rc500');
  const rcCmds = await page.$$eval('.press[data-slot="1"] select[data-choice] option', o => o.map(x => x.value));
  check('the command list follows the chosen pedal (RC-500)', rcCmds.includes('rc-next') && !rcCmds.includes('hx-tap'), rcCmds.join(','));
  await page.select('.press[data-slot="1"] select[data-choice]', 'rc-next');
  await page.click('#legend li[data-b="2"]');
  await page.select('.press[data-slot="5"] select[data-dev]', 'any');
  await page.select('.press[data-slot="5"] select[data-choice]', 'toggle');
  await page.$eval('.press[data-slot="5"] input[data-f="0"]', e => { e.value = '80'; e.dispatchEvent(new Event('change')); });
  await page.select('.press[data-slot="5"] select[data-ch]', '10');
  await page.select('#channel', '1');
  check('unsaved marker shows', !(await page.$eval('#dirty', e => e.hidden)));
  await page.click('#btnSave');
  const sm = await waitText('#pedalMsg', /✓|failed/, 15000);
  check('saves to the pedal: ' + sm, /✓/.test(sm), sm);
  const want = await page.evaluate(() => Array.from(__mm.encode(__mm.state.cfg), b => b.toString(16).padStart(2, '0')).join('').toUpperCase());
  const ee = await simQuery('eeprom 53', 'EE ');
  check('pedal EEPROM matches the editor', ee === want, `\n ee   ${ee}\n want ${want}`);
  check('switch 1 short = CC 64/127, long = next 0..98, switch 3 long = toggle CC80 ch10',
    want.slice(24, 34) === '0200407F00' && want.slice(34, 44) === '0500006200' && want.slice(74, 84) === '030A507F00', want);
  await page.screenshot({path: `${SHOTS}/${tag}-3-edited.png`, fullPage: true});

  // 5. HX template
  await page.select('#template', 'hx'); await page.click('#btnTemplate');
  check('HX template switches profile', (await page.$eval('#profile', e => e.value)) === '1');
  await page.click('#legend li[data-b="0"]');
  check('HX profile explains UP/DN', /MIDI Muppet HX/.test(await text('#btnBody')));
  await page.click('#btnSave');
  check('HX setup saved', /✓/.test(await waitText('#pedalMsg', /✓|failed/, 15000)));
  await page.screenshot({path: `${SHOTS}/${tag}-4-hx.png`, fullPage: true});

  // 6. read back after reconnect
  txLog = [];
  await page.click('#btnDisconnect'); await sleep(800);
  check('disconnects', (await text('#statusText')) === 'Not connected');
  pending.push('baud 31250');          // listen on the MIDI side now
  await sleep(1500);
  pending.push('pin 3 0'); await sleep(150); pending.push('pin 3 1'); await sleep(300);
  check('after disconnect the pedal plays: tap tempo on switch 3 (MIDI channel 2)', txLog.join('').endsWith('B1407F'), txLog.join(''));
  await page.click('#btnConnect');
  check('reconnects', (await waitText('#statusText', /^Connected|Not connected/, 20000)) === 'Connected');
  check('reads the HX setup back', (await page.$eval('#profile', e => e.value)) === '1');

  // 7. export / import round trip
  const exported = await page.evaluate(() => JSON.stringify({format: 'midi-muppet-max', setup: Array.from(__mm.encode(__mm.state.cfg), b => b.toString(16).padStart(2, '0')).join('')}));
  check('export format is plain JSON', JSON.parse(exported).setup.length === 106);

  // 7b. original two button MIDI Muppet HX
  await page.select('#template', 'original'); await page.click('#btnTemplate');
  check('original template: pedal is the 2 switch MIDI Muppet HX', (await page.$eval('#hardware', e => e.value)) === '2');
  check('original template: picture and list show 2 switches',
    (await page.$$eval('#pedal .sw', l => l.length)) === 2 && (await page.$$eval('#legend li', l => l.length)) === 2);
  const lr = await page.$$eval('#legend li small', l => l.map(x => x.textContent));
  check('original template: UP on the right, DN on the left', /^Right/.test(lr[0]) && /^Left/.test(lr[1]), lr.join(' | '));
  await page.screenshot({path: `${SHOTS}/${tag}-6-original.png`, fullPage: true});
  await page.click('#btnSave');
  check('original setup saved', /✓/.test(await waitText('#pedalMsg', /✓|failed/, 15000)));
  const ee2 = await simQuery('eeprom 53', 'EE ');
  check('pedal stores two-switch flag and HX profile', (parseInt(ee2.slice(10, 12), 16) & 4) === 4 && ee2.slice(6, 8) === '01', ee2);
  await page.select('#hardware', '4');
  check('back to 4 switches shows 4 switches', (await page.$$eval('#pedal .sw', l => l.length)) === 4);

  // 8. mobile layout
  await page.setViewport({width: 390, height: 900, isMobile: true});
  await sleep(300);
  await page.screenshot({path: `${SHOTS}/${tag}-5-mobile.png`, fullPage: true});
  check('no script errors during the test', errors.length === 0, errors.join('; '));

  await browser.close();
  clearInterval(timer); sim.stdin.write('quit\n');
  console.log(`\n${tag}: ${failures} failures`);
  process.exit(failures ? 1 : 0);
})().catch(e => { console.error('CRASH', e); process.exit(2); });
