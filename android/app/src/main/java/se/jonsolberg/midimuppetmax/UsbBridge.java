/*
 * MIDI Muppet Max – Android app
 * Copyright (C) 2026 Jon Solberg – GPL v3 or later, see LICENSE.
 */
package se.jonsolberg.midimuppetmax;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.util.Base64;
import android.webkit.JavascriptInterface;
import android.webkit.WebView;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * The native half of the app's WebUSB (assets/android-usb.js): the editor calls
 * window.AndroidUsb.call(id, method, argsJson) and gets the answer back through
 * window.__usbDone(id, ok, value). Everything runs off the UI thread, because USB
 * transfers block.
 */
class UsbBridge {
    private static final String ACTION_PERMISSION = "se.jonsolberg.midimuppetmax.USB_PERMISSION";
    /** USB serial chips on Arduinos and adapters, as in the editor's USB_SERIAL_FILTERS */
    private static final int[] VENDORS = {0x1a86, 0x0403, 0x10c4, 0x2341, 0x2a03};

    private final Activity activity;
    private final WebView web;
    private final UsbManager usb;
    private final ExecutorService pool = Executors.newCachedThreadPool();
    private final Map<String, Open> open = new HashMap<>();
    private PendingRequest waiting;

    private static class Open {
        UsbDevice device;
        UsbDeviceConnection conn;
        final Map<Integer, UsbInterface> claimed = new HashMap<>();
        volatile boolean closed;
    }

    private static class PendingRequest {
        final int id;
        final UsbDevice device;
        PendingRequest(int id, UsbDevice device) { this.id = id; this.device = device; }
    }

    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            UsbDevice d = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
            if (ACTION_PERMISSION.equals(intent.getAction())) {
                PendingRequest p = waiting;
                waiting = null;
                if (p == null) return;
                boolean ok = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
                if (ok) finish(p.id, true, info(p.device).toString());
                else finish(p.id, false, "USB permission was not granted.");
            } else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(intent.getAction()) && d != null) {
                Open o;
                synchronized (open) { o = open.remove(d.getDeviceName()); }
                if (o != null) closeQuietly(o);
                web.post(() -> web.evaluateJavascript("window.__usbDetached&&__usbDetached(" + JSONObject.quote(d.getDeviceName()) + ")", null));
            }
        }
    };

    UsbBridge(Activity activity, WebView web) {
        this.activity = activity;
        this.web = web;
        this.usb = (UsbManager) activity.getSystemService(Context.USB_SERVICE);
        IntentFilter f = new IntentFilter(ACTION_PERMISSION);
        f.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        if (Build.VERSION.SDK_INT >= 33) activity.registerReceiver(receiver, f, Context.RECEIVER_NOT_EXPORTED);
        else activity.registerReceiver(receiver, f);
    }

    void destroy() {
        try { activity.unregisterReceiver(receiver); } catch (IllegalArgumentException ignored) { }
        synchronized (open) {
            for (Open o : open.values()) closeQuietly(o);
            open.clear();
        }
        pool.shutdownNow();
    }

    @JavascriptInterface
    public void call(int id, String method, String argsJson) {
        pool.execute(() -> {
            try {
                JSONArray a = new JSONArray(argsJson);
                Object result = run(id, method, a);
                if (result != null) finish(id, true, result);
            } catch (Exception e) {
                finish(id, false, e.getMessage() != null ? e.getMessage() : e.toString());
            }
        });
    }

    /** Returns the answer, or null when it comes later (the permission dialog). */
    private Object run(int id, String method, JSONArray a) throws Exception {
        switch (method) {
            case "request": return request(id, new JSONArray(a.getString(0)));
            case "granted": {
                JSONArray list = new JSONArray();
                for (UsbDevice d : usb.getDeviceList().values()) if (usb.hasPermission(d) && serial(d)) list.put(info(d));
                return list.toString();
            }
            case "open": {
                UsbDevice d = find(a.getString(0));
                synchronized (open) {
                    Open o = open.get(d.getDeviceName());
                    if (o != null && !o.closed) return true;
                    UsbDeviceConnection c = usb.openDevice(d);
                    if (c == null) throw new Exception("Could not open the USB device.");
                    o = new Open();
                    o.device = d;
                    o.conn = c;
                    open.put(d.getDeviceName(), o);
                }
                return true;
            }
            case "close": {
                Open o;
                synchronized (open) { o = open.remove(a.getString(0)); }
                if (o != null) closeQuietly(o);
                return true;
            }
            case "claim": {
                Open o = get(a.getString(0));
                int n = a.getInt(1);
                UsbInterface itf = iface(o.device, n);
                if (!o.conn.claimInterface(itf, true)) throw new Exception("Could not claim USB interface " + n + ".");
                o.claimed.put(n, itf);
                return true;
            }
            case "release": {
                Open o = get(a.getString(0));
                UsbInterface itf = o.claimed.remove(a.getInt(1));
                if (itf != null) o.conn.releaseInterface(itf);
                return true;
            }
            case "ctrl": {
                Open o = get(a.getString(0));
                int type = a.getInt(1), request = a.getInt(2), value = a.getInt(3), index = a.getInt(4);
                String data = a.getString(5);
                int length = a.getInt(6);
                if ((type & 0x80) != 0) {
                    byte[] buf = new byte[Math.max(length, 0)];
                    int r = o.conn.controlTransfer(type, request, value, index, buf, buf.length, 1000);
                    if (r < 0) throw new Exception("USB control transfer failed.");
                    return Base64.encodeToString(buf, 0, r, Base64.NO_WRAP);
                }
                byte[] out = data.isEmpty() ? null : Base64.decode(data, Base64.DEFAULT);
                int r = o.conn.controlTransfer(type, request, value, index, out, out == null ? 0 : out.length, 1000);
                if (r < 0) throw new Exception("USB control transfer failed.");
                return r;
            }
            case "bulkIn": {
                Open o = get(a.getString(0));
                UsbEndpoint ep = endpoint(o, a.getInt(1), UsbConstants.USB_DIR_IN);
                byte[] buf = new byte[Math.max(a.getInt(2), ep.getMaxPacketSize())];
                // Short timeout: an empty answer just means "nothing yet", the editor asks again.
                int r = o.conn.bulkTransfer(ep, buf, buf.length, 100);
                if (o.closed) throw new Exception("The USB device was closed.");
                return r > 0 ? Base64.encodeToString(buf, 0, r, Base64.NO_WRAP) : "";
            }
            case "bulkOut": {
                Open o = get(a.getString(0));
                UsbEndpoint ep = endpoint(o, a.getInt(1), UsbConstants.USB_DIR_OUT);
                byte[] data = Base64.decode(a.getString(2), Base64.DEFAULT);
                int sent = 0;
                while (sent < data.length) {
                    int n = Math.min(data.length - sent, 16384);
                    byte[] part = new byte[n];
                    System.arraycopy(data, sent, part, 0, n);
                    int r = o.conn.bulkTransfer(ep, part, n, 2000);
                    if (r < 0) throw new Exception("USB write failed.");
                    sent += r;
                }
                return sent;
            }
            default: throw new Exception("Unknown USB call " + method);
        }
    }

    /** Find a USB serial device that matches the editor's filters and ask for permission. */
    private Object request(int id, JSONArray filters) throws Exception {
        UsbDevice found = null;
        for (UsbDevice d : usb.getDeviceList().values()) {
            if (matches(d, filters)) { found = d; break; }
        }
        if (found == null) throw new Exception("No Arduino found. Connect the pedal's USB port to the phone with a USB OTG cable.");
        if (usb.hasPermission(found)) return info(found).toString();
        waiting = new PendingRequest(id, found);
        Intent i = new Intent(ACTION_PERMISSION).setPackage(activity.getPackageName());
        int flags = Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0;
        usb.requestPermission(found, PendingIntent.getBroadcast(activity, 0, i, flags));
        return null;
    }

    private static boolean serial(UsbDevice d) {
        for (int v : VENDORS) if (d.getVendorId() == v) return true;
        if (d.getDeviceClass() == UsbConstants.USB_CLASS_COMM) return true;
        for (int i = 0; i < d.getInterfaceCount(); i++) {
            if (d.getInterface(i).getInterfaceClass() == UsbConstants.USB_CLASS_COMM) return true;
        }
        return false;
    }

    private static boolean matches(UsbDevice d, JSONArray filters) throws JSONException {
        if (filters.length() == 0) return serial(d);
        for (int k = 0; k < filters.length(); k++) {
            JSONObject f = filters.getJSONObject(k);
            boolean ok = true;
            if (f.has("vendorId") && f.getInt("vendorId") != d.getVendorId()) ok = false;
            if (f.has("productId") && f.getInt("productId") != d.getProductId()) ok = false;
            if (f.has("classCode")) {
                int c = f.getInt("classCode");
                boolean any = d.getDeviceClass() == c;
                for (int i = 0; i < d.getInterfaceCount(); i++) if (d.getInterface(i).getInterfaceClass() == c) any = true;
                if (!any) ok = false;
            }
            if (ok) return true;
        }
        return false;
    }

    /** The device as WebUSB describes it: one configuration, its interfaces and endpoints. */
    private static JSONObject info(UsbDevice d) {
        try {
            JSONArray itfs = new JSONArray();
            Map<Integer, Boolean> seen = new HashMap<>();
            for (int i = 0; i < d.getInterfaceCount(); i++) {
                UsbInterface itf = d.getInterface(i);
                if (seen.containsKey(itf.getId())) continue;   // first alternate setting only
                seen.put(itf.getId(), true);
                JSONArray eps = new JSONArray();
                for (int e = 0; e < itf.getEndpointCount(); e++) {
                    UsbEndpoint ep = itf.getEndpoint(e);
                    String type;
                    switch (ep.getType()) {
                        case UsbConstants.USB_ENDPOINT_XFER_BULK: type = "bulk"; break;
                        case UsbConstants.USB_ENDPOINT_XFER_INT: type = "interrupt"; break;
                        case UsbConstants.USB_ENDPOINT_XFER_ISOC: type = "isochronous"; break;
                        default: type = "control";
                    }
                    eps.put(new JSONObject()
                            .put("endpointNumber", ep.getEndpointNumber())
                            .put("direction", ep.getDirection() == UsbConstants.USB_DIR_IN ? "in" : "out")
                            .put("type", type)
                            .put("packetSize", ep.getMaxPacketSize()));
                }
                itfs.put(new JSONObject()
                        .put("interfaceNumber", itf.getId())
                        .put("alternate", new JSONObject().put("interfaceClass", itf.getInterfaceClass()).put("endpoints", eps)));
            }
            return new JSONObject()
                    .put("id", d.getDeviceName())
                    .put("vendorId", d.getVendorId())
                    .put("productId", d.getProductId())
                    .put("productName", d.getProductName() == null ? "" : d.getProductName())
                    .put("configuration", new JSONObject().put("configurationValue", 1).put("interfaces", itfs));
        } catch (JSONException e) {
            throw new RuntimeException(e);
        }
    }

    private UsbDevice find(String name) throws Exception {
        UsbDevice d = usb.getDeviceList().get(name);
        if (d == null) throw new Exception("The USB device is no longer connected.");
        return d;
    }

    private Open get(String name) throws Exception {
        Open o;
        synchronized (open) { o = open.get(name); }
        if (o == null || o.closed) throw new Exception("The USB device is not open.");
        return o;
    }

    private static UsbInterface iface(UsbDevice d, int number) throws Exception {
        for (int i = 0; i < d.getInterfaceCount(); i++) {
            if (d.getInterface(i).getId() == number) return d.getInterface(i);
        }
        throw new Exception("No USB interface " + number + ".");
    }

    private static UsbEndpoint endpoint(Open o, int number, int dir) throws Exception {
        for (UsbInterface itf : o.claimed.values()) {
            for (int e = 0; e < itf.getEndpointCount(); e++) {
                UsbEndpoint ep = itf.getEndpoint(e);
                if (ep.getEndpointNumber() == number && ep.getDirection() == dir) return ep;
            }
        }
        throw new Exception("No USB endpoint " + number + ".");
    }

    private static void closeQuietly(Open o) {
        o.closed = true;
        try {
            for (UsbInterface itf : o.claimed.values()) o.conn.releaseInterface(itf);
            o.conn.close();
        } catch (Exception ignored) { }
    }

    private void finish(int id, boolean ok, Object value) {
        String v = value instanceof String ? JSONObject.quote((String) value) : String.valueOf(value);
        String js = "window.__usbDone&&__usbDone(" + id + "," + ok + "," + v + ")";
        web.post(() -> web.evaluateJavascript(js, null));
    }
}
