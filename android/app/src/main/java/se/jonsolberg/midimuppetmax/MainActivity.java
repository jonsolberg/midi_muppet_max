/*
 * MIDI Muppet Max – Android app
 * Copyright (C) 2026 Jon Solberg – GPL v3 or later, see LICENSE.
 */
package se.jonsolberg.midimuppetmax;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.webkit.JavascriptInterface;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Toast;

import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

/**
 * The MIDI Muppet Max editor (the same page as the web editor, bundled in assets) in a WebView,
 * with USB serial through Android's USB host API (UsbBridge + assets/android-usb.js).
 * The app has no INTERNET permission: links to GitHub, jonsolberg.se and Ko-fi open in the browser.
 */
public class MainActivity extends Activity {
    private static final int PICK_FILE = 1, SAVE_FILE = 2;

    private WebView web;
    private UsbBridge usb;
    private ValueCallback<Uri[]> fileCallback;
    private String pendingSave;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        web = new WebView(this);
        setContentView(web);
        if (savedInstanceState == null) UpdateChecker.start(this);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);          // the editor keeps the current setup in localStorage
        s.setAllowFileAccess(true);            // the bundled page in file:///android_asset/
        s.setAllowContentAccess(false);
        s.setBlockNetworkLoads(true);
        s.setTextZoom(100);

        web.setWebViewClient(new WebViewClient() {
            @Override
            public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                Uri u = request.getUrl();
                String scheme = u.getScheme();
                if ("http".equals(scheme) || "https".equals(scheme) || "mailto".equals(scheme)) {
                    try { startActivity(new Intent(Intent.ACTION_VIEW, u)); }
                    catch (ActivityNotFoundException ignored) { }
                    return true;
                }
                return !"file".equals(scheme);
            }
        });
        // "Import setup": the editor's <input type="file">
        web.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onShowFileChooser(WebView view, ValueCallback<Uri[]> callback, FileChooserParams params) {
                if (fileCallback != null) fileCallback.onReceiveValue(null);
                fileCallback = callback;
                Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
                try {
                    startActivityForResult(i, PICK_FILE);
                } catch (ActivityNotFoundException e) {
                    fileCallback = null;
                    return false;
                }
                return true;
            }
        });

        usb = new UsbBridge(this, web);
        web.addJavascriptInterface(usb, "AndroidUsb");
        web.addJavascriptInterface(new App(), "AndroidApp");

        if (savedInstanceState != null) web.restoreState(savedInstanceState);
        else web.loadUrl("file:///android_asset/index.html");
    }

    /** Called from the editor as window.AndroidApp.* */
    private class App {
        /** "Export setup": save the file where the user chooses. */
        @JavascriptInterface
        public void saveFile(String name, String content) {
            runOnUiThread(() -> {
                pendingSave = content;
                Intent i = new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                        .setType("application/json").putExtra(Intent.EXTRA_TITLE, name);
                try { startActivityForResult(i, SAVE_FILE); }
                catch (ActivityNotFoundException e) { pendingSave = null; }
            });
        }

        @JavascriptInterface
        public String version() {
            return BuildConfig.VERSION_NAME;
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        Uri uri = resultCode == RESULT_OK && data != null ? data.getData() : null;
        if (requestCode == PICK_FILE && fileCallback != null) {
            fileCallback.onReceiveValue(uri == null ? null : new Uri[]{uri});
            fileCallback = null;
        } else if (requestCode == SAVE_FILE && pendingSave != null) {
            if (uri != null) {
                try (OutputStream out = getContentResolver().openOutputStream(uri)) {
                    if (out != null) out.write(pendingSave.getBytes(StandardCharsets.UTF_8));
                    Toast.makeText(this, R.string.saved, Toast.LENGTH_SHORT).show();
                } catch (Exception e) {
                    Toast.makeText(this, getString(R.string.save_failed, e.getMessage()), Toast.LENGTH_LONG).show();
                }
            }
            pendingSave = null;
        }
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        web.saveState(outState);
    }

    @Override
    protected void onDestroy() {
        usb.destroy();
        web.destroy();
        super.onDestroy();
    }
}
