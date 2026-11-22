/*
 * MIDI Muppet Max – Android app
 * Copyright (C) 2026 Jon Solberg – GPL v3 or later, see LICENSE.
 *
 * WebUSB for the app's WebView. A WebView has neither Web Serial nor WebUSB, so this gives the
 * editor a navigator.usb that is backed by Android's own USB host API (window.AndroidUsb, see
 * UsbBridge.java). The editor's USB serial drivers (CH340, FTDI, CP210x, CDC-ACM) run unchanged.
 */
(function () {
  if (!window.AndroidUsb) return;   // only in the app; there it replaces any navigator.usb
  const pending = new Map();
  let seq = 0;
  window.__usbDone = (id, ok, value) => {
    const p = pending.get(id);
    if (!p) return;
    pending.delete(id);
    if (ok) p.resolve(value); else p.reject(new DOMException(String(value), 'NetworkError'));
  };
  const call = (method, ...args) => new Promise((resolve, reject) => {
    const id = ++seq;
    pending.set(id, {resolve, reject});
    window.AndroidUsb.call(id, method, JSON.stringify(args));
  });
  const toU8 = d => d instanceof ArrayBuffer ? new Uint8Array(d)
    : ArrayBuffer.isView(d) ? new Uint8Array(d.buffer, d.byteOffset, d.byteLength) : new Uint8Array(0);
  const b64 = u8 => { let s = ''; for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]); return btoa(s); };
  const unb64 = s => { const t = atob(s || ''), u = new Uint8Array(t.length); for (let i = 0; i < t.length; i++) u[i] = t.charCodeAt(i); return u; };
  const view = u8 => new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
  const TYPE = {standard: 0, class: 1, vendor: 2}, RECIPIENT = {device: 0, interface: 1, endpoint: 2, other: 3};
  const requestType = (s, dirIn) => (dirIn ? 0x80 : 0) | (TYPE[s.requestType] << 5) | RECIPIENT[s.recipient];

  const devices = new Map();
  class AndroidUsbDevice {
    constructor(info) { Object.assign(this, info); this.opened = false; }
    async open() { await call('open', this.id); this.opened = true; }
    async close() { this.opened = false; await call('close', this.id); }
    async selectConfiguration() {}
    async claimInterface(n) { await call('claim', this.id, n); }
    async releaseInterface(n) { await call('release', this.id, n); }
    async clearHalt() {}
    async controlTransferOut(s, data) {
      const u8 = data ? toU8(data) : null;
      const n = await call('ctrl', this.id, requestType(s, false), s.request, s.value, s.index, u8 ? b64(u8) : '', u8 ? u8.length : 0);
      return {status: 'ok', bytesWritten: n};
    }
    async controlTransferIn(s, length) {
      const r = await call('ctrl', this.id, requestType(s, true), s.request, s.value, s.index, '', length);
      return {status: 'ok', data: view(unb64(r))};
    }
    async transferIn(endpoint, length) {
      const r = await call('bulkIn', this.id, endpoint, length);
      return {status: 'ok', data: view(unb64(r))};
    }
    async transferOut(endpoint, data) {
      const u8 = toU8(data);
      const n = await call('bulkOut', this.id, endpoint, b64(u8));
      return {status: 'ok', bytesWritten: n};
    }
  }
  const wrap = info => { let d = devices.get(info.id); if (!d) { d = new AndroidUsbDevice(info); devices.set(info.id, d); } return d; };

  const usb = new EventTarget();
  usb.requestDevice = async ({filters} = {}) => wrap(JSON.parse(await call('request', JSON.stringify(filters || []))));
  usb.getDevices = async () => JSON.parse(await call('granted')).map(wrap);
  window.__usbDetached = id => {
    const d = devices.get(id);
    if (!d) return;
    devices.delete(id);
    d.opened = false;
    const e = new Event('disconnect');
    e.device = d;
    usb.dispatchEvent(e);
  };
  Object.defineProperty(navigator, 'usb', {value: usb, configurable: true});
})();
