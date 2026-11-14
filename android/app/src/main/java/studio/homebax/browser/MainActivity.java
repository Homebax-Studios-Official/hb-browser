package studio.homebax.browser;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.DownloadManager;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.net.Uri;
import android.net.http.SslError;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.webkit.CookieManager;
import android.webkit.DownloadListener;
import android.webkit.GeolocationPermissions;
import android.webkit.PermissionRequest;
import android.webkit.SslErrorHandler;
import android.webkit.URLUtil;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** HB Browser for Android: system WebView only, no libraries. Tabs, split view, plugins, safe downloads. */
public class MainActivity extends Activity {
    static final String VERSION = "3.1.0";
    static final String UPDATE_URL = "https://homebax.vercel.app/browser/info.json";
    static final String[][] ENGINES = {
        {"google", "Google", "https://www.google.com/search?q=%s"},
        {"duckduckgo", "DuckDuckGo", "https://duckduckgo.com/?q=%s"},
        {"bing", "Bing", "https://www.bing.com/search?q=%s"},
        {"brave", "Brave Search", "https://search.brave.com/search?q=%s"},
        {"seznam", "Seznam", "https://search.seznam.cz/?q=%s"}};
    static final String MATCHER =
        "function M(p,u){if(p==='<all_urls>')p='*://*/*';var m=/^(\\*|https?):\\/\\/([^\\/]*)(\\/.*)$/.exec(p);if(!m)return false;"
        + "var x;try{x=new URL(u)}catch(e){return false}var pr=x.protocol.slice(0,-1);if(pr!=='http'&&pr!=='https')return false;"
        + "if(m[1]!=='*'&&m[1]!==pr)return false;var h=x.hostname.toLowerCase(),ph=m[2].toLowerCase();if(h==='hb.local')return false;"
        + "if(ph!=='*'){if(ph.slice(0,2)==='*.'){var b=ph.slice(2);if(h!==b&&!h.endsWith('.'+b))return false}else if(h!==ph)return false}"
        + "return new RegExp('^'+m[3].replace(/[.+?^${}()|[\\]\\\\]/g,'\\\\$&').replace(/\\*/g,'.*')+'$').test(x.pathname+x.search)}";
    static final String[] TRACK = {"fbclid", "gclid", "gbraid", "wbraid", "dclid", "msclkid", "mc_eid", "mc_cid", "igshid", "yclid",
        "twclid", "ttclid", "_ga", "_gl", "_hsenc", "_hsmi", "vero_id", "oly_enc_id", "oly_anon_id"};
    static final String[] DANGEROUS = {"exe", "com", "scr", "pif", "bat", "cmd", "msi", "lnk", "js", "jse", "vbs", "vbe", "wsf", "ps1",
        "reg", "hta", "cpl", "jar", "dll", "apk", "apks", "xapk", "sh", "appx", "msix", "iso"};

    SharedPreferences prefs;
    EditText urlBar;
    LinearLayout content;
    final ArrayList<WebView> tabs = new ArrayList<>();
    int first = 0, second = -1, focus = 0;      // pane 1 tab, pane 2 tab (-1 = no split), focused tab
    float ratio = 0.5f;
    boolean side = false;                         // false = stacked (phone), true = side by side
    boolean updateShown = false;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = getSharedPreferences("hb", MODE_PRIVATE);
        side = getResources().getConfiguration().orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE;
        CookieManager.getInstance().setAcceptCookie(true);
        if (BuildConfig.DEBUG) WebView.setWebContentsDebuggingEnabled(true);   // desktop DevTools via chrome://inspect (debug builds only)

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        LinearLayout bar = new LinearLayout(this);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.addView(btn("<", v -> { WebView w = cur(); if (w != null && w.canGoBack()) w.goBack(); }));
        bar.addView(btn(">", v -> { WebView w = cur(); if (w != null && w.canGoForward()) w.goForward(); }));
        bar.addView(btn("R", v -> { WebView w = cur(); if (w != null) w.reload(); }));
        urlBar = new EditText(this);
        urlBar.setSingleLine(true);
        urlBar.setHint("Search or enter address");
        urlBar.setImeOptions(EditorInfo.IME_ACTION_GO);
        urlBar.setOnEditorActionListener((v, a, e) -> { go(urlBar.getText().toString()); hideKeyboard(); return true; });
        bar.addView(urlBar, new LinearLayout.LayoutParams(0, -2, 1));
        bar.addView(btn("Tabs", v -> showTabs()));
        bar.addView(btn("...", v -> showMenu()));
        content = new LinearLayout(this);
        content.setOrientation(side ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        root.addView(bar, new LinearLayout.LayoutParams(-1, -2));
        root.addView(content, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(root);

        Uri data = getIntent() != null ? getIntent().getData() : null;
        newTab(data != null ? data.toString() : "");
        checkUpdate();
    }

    Button btn(String t, View.OnClickListener l) {
        Button b = new Button(this);
        b.setText(t);
        b.setAllCaps(false);
        b.setMinWidth(0); b.setMinimumWidth(0);
        b.setOnClickListener(l);
        return b;
    }

    WebView cur() { return focus >= 0 && focus < tabs.size() ? tabs.get(focus) : null; }

    // ---------------------------------------------------------------- address handling
    String engineTemplate() {
        String id = prefs.getString("engine", "google");
        if (id.equals("custom")) { String c = prefs.getString("customUrl", ""); if (validTemplate(c)) return c; }
        for (String[] e : ENGINES) if (e[0].equals(id)) return e[2];
        return ENGINES[0][2];
    }

    static boolean validTemplate(String u) {
        if (u == null || u.length() > 2048 || !(u.startsWith("https://") || u.startsWith("http://"))) return false;
        for (int i = 0; i < u.length(); i++) if (u.charAt(i) <= 0x20) return false;
        int p = u.indexOf("%s");
        return p > 0 && u.indexOf("%s", p + 2) < 0;
    }

    String resolve(String in) {
        in = in.trim();
        if (in.isEmpty()) return null;
        String lower = in.toLowerCase(Locale.ROOT);
        if (lower.startsWith("http://") || lower.startsWith("https://")) return in;
        boolean spaced = in.indexOf(' ') >= 0;
        if (!spaced && !lower.contains(":") && (in.contains(".") || lower.startsWith("localhost"))) return "https://" + in;
        return engineTemplate().replace("%s", Uri.encode(in));
    }

    static String stripTracking(String url) {
        int q = url.indexOf('?');
        if (q < 0) return url;
        int h = url.indexOf('#', q);
        String query = url.substring(q + 1, h < 0 ? url.length() : h);
        StringBuilder kept = new StringBuilder();
        for (String part : query.split("&")) {
            if (part.isEmpty()) continue;
            String key = part.split("=", 2)[0].toLowerCase(Locale.ROOT);
            boolean track = key.startsWith("utm_");
            for (String t : TRACK) track |= key.equals(t);
            if (!track) kept.append(kept.length() > 0 ? "&" : "").append(part);
        }
        return url.substring(0, q) + (kept.length() > 0 ? "?" + kept : "") + (h < 0 ? "" : url.substring(h));
    }

    void go(String input) {
        WebView w = cur();
        if (w == null) return;
        String u = resolve(input);
        if (u == null) showNewTab(w); else w.loadUrl(u);
    }

    void showNewTab(WebView w) {
        String html = "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>HB Browser</title>"
            + "<style>body{margin:0;height:100vh;display:grid;place-items:center;background:#14161c;color:#e8eaf0;font:16px system-ui}"
            + "input{width:86vw;box-sizing:border-box;padding:14px 20px;border-radius:26px;border:1px solid #333a4a;background:#1d2029;color:inherit;font-size:18px;outline:none}"
            + "h1{font-weight:600;text-align:center;margin:0 0 22px}</style><div><h1>HB Browser</h1><input id=q placeholder='Search or enter address'></div>"
            + "<script>q.onkeydown=function(e){if(e.key==='Enter'&&q.value.trim())location.href='hb-go:'+encodeURIComponent(q.value.trim())}</script>";
        w.loadDataWithBaseURL("about:blank", html, "text/html", "utf-8", null);
    }

    // ---------------------------------------------------------------- tabs and split view
    WebView newTab(String input) {
        WebView w = makeWebView();
        tabs.add(w);
        second = -1;
        first = focus = tabs.size() - 1;
        layoutPanes();
        String u = resolve(input);
        if (u == null) showNewTab(w); else w.loadUrl(u);
        return w;
    }

    void layoutPanes() {
        content.removeAllViews();
        content.setOrientation(side ? LinearLayout.HORIZONTAL : LinearLayout.VERTICAL);
        if (second >= 0 && second < tabs.size() && second != first) {
            float r = Math.max(0.2f, Math.min(0.8f, ratio));
            content.addView(tabs.get(first), new LinearLayout.LayoutParams(side ? 0 : -1, side ? -1 : 0, r));
            View gap = new View(this);
            gap.setBackgroundColor(0xFF4F7CFF);
            content.addView(gap, new LinearLayout.LayoutParams(side ? 4 : -1, side ? -1 : 4));
            content.addView(tabs.get(second), new LinearLayout.LayoutParams(side ? 0 : -1, side ? -1 : 0, 1 - r));
        } else {
            second = -1;
            first = focus = Math.min(focus, tabs.size() - 1);
            content.addView(tabs.get(first), new LinearLayout.LayoutParams(-1, -1));
        }
        syncUrl();
    }

    void toggleSplit() {
        if (second >= 0) { second = -1; layoutPanes(); return; }
        if (tabs.size() < 2) newTabKeepingSplit();
        int other = (first + 1) % tabs.size();
        second = other;
        layoutPanes();
    }

    void newTabKeepingSplit() {
        WebView w = makeWebView();
        tabs.add(w);
        showNewTab(w);
    }

    void selectTab(int i) {
        if (second >= 0 && (i == first || i == second)) { focus = i; syncUrl(); return; }
        second = -1;
        first = focus = i;
        layoutPanes();
    }

    void closeTab(int i) {
        WebView w = tabs.remove(i);
        w.destroy();
        if (tabs.isEmpty()) { finish(); return; }
        second = -1;
        first = focus = Math.min(i, tabs.size() - 1);
        layoutPanes();
    }

    void showTabs() {
        List<String> names = new ArrayList<>();
        for (WebView w : tabs) names.add((w == cur() ? "> " : "") + (w.getTitle() == null || w.getTitle().isEmpty() ? "New tab" : w.getTitle()));
        names.add("+ New tab");
        names.add("x Close current tab");
        new AlertDialog.Builder(this).setTitle("Tabs").setItems(names.toArray(new String[0]), (d, i) -> {
            if (i < tabs.size()) selectTab(i);
            else if (i == tabs.size()) newTab("");
            else closeTab(focus);
        }).show();
    }

    void syncUrl() {
        WebView w = cur();
        String u = w == null ? null : w.getUrl();
        urlBar.setText(u == null || u.startsWith("about:") || u.startsWith("data:") ? "" : u);
    }

    // ---------------------------------------------------------------- web view
    WebView makeWebView() {
        WebView w = new WebView(this);
        WebSettings s = w.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setAllowFileAccess(false);
        s.setAllowContentAccess(false);
        s.setAllowFileAccessFromFileURLs(false);
        s.setAllowUniversalAccessFromFileURLs(false);
        s.setMixedContentMode(WebSettings.MIXED_CONTENT_NEVER_ALLOW);
        s.setSupportMultipleWindows(false);
        s.setJavaScriptCanOpenWindowsAutomatically(false);
        s.setGeolocationEnabled(false);
        s.setBuiltInZoomControls(true);
        s.setDisplayZoomControls(false);
        if (Build.VERSION.SDK_INT >= 26) { s.setSafeBrowsingEnabled(true); w.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_YES); }
        CookieManager.getInstance().setAcceptThirdPartyCookies(w, false);
        w.setOnTouchListener((v, e) -> {
            if (e.getAction() == MotionEvent.ACTION_DOWN) { int i = tabs.indexOf(v); if (i >= 0 && i != focus && (i == first || i == second)) { focus = i; syncUrl(); } }
            return false;
        });
        w.setWebViewClient(new WebViewClient() {
            @Override public boolean shouldOverrideUrlLoading(WebView v, WebResourceRequest r) {
                Uri u = r.getUrl();
                String sc = u.getScheme() == null ? "" : u.getScheme().toLowerCase(Locale.ROOT);
                if (sc.equals("hb-go")) { v.loadUrl(resolve(Uri.decode(u.getSchemeSpecificPart()))); return true; }
                if (sc.equals("http") || sc.equals("https")) {
                    if (r.hasGesture() && prefs.getBoolean("cleanUrls", true)) {
                        String c = stripTracking(u.toString());
                        if (!c.equals(u.toString())) { v.loadUrl(c); return true; }
                    }
                    return false;
                }
                if (r.hasGesture() && (sc.equals("mailto") || sc.equals("tel"))) {
                    try { startActivity(new Intent(Intent.ACTION_VIEW, u)); } catch (Exception ignored) { }
                }
                return true;   // every other scheme is blocked
            }
            @Override public void onPageStarted(WebView v, String url, Bitmap f) { if (v == cur()) syncUrl(); }
            @Override public void onPageCommitVisible(WebView v, String url) { injectPlugins(v, url); }
            @Override public void onPageFinished(WebView v, String url) {
                CookieManager.getInstance().flush();
                if (v == cur()) syncUrl();
                if (url != null && url.startsWith("http")) addHistory(url, v.getTitle());
            }
            @Override public void onReceivedSslError(WebView v, SslErrorHandler h, SslError e) { h.cancel(); }   // never bypass certificate errors
        });
        w.setWebChromeClient(new WebChromeClient() {
            @Override public void onPermissionRequest(PermissionRequest r) { r.deny(); }
            @Override public void onGeolocationPermissionsShowPrompt(String o, GeolocationPermissions.Callback c) { c.invoke(o, false, false); }
        });
        w.setDownloadListener((url, ua, cd, mime, len) -> download(url, ua, cd, mime));
        return w;
    }

    void download(String url, String ua, String cd, String mime) {
        String name = safeName(URLUtil.guessFileName(url, cd, mime));
        if (url.startsWith("http://") && prefs.getBoolean("blockInsecureDownloads", true) && dangerous(name)) {
            Toast.makeText(this, "Blocked: program downloaded over an insecure connection", Toast.LENGTH_LONG).show();
            return;
        }
        if (!(url.startsWith("https://") || url.startsWith("http://"))) return;
        DownloadManager.Request rq = new DownloadManager.Request(Uri.parse(url));
        rq.setMimeType(mime);
        rq.addRequestHeader("User-Agent", ua);
        rq.setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED);
        rq.setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, name);
        ((DownloadManager) getSystemService(Context.DOWNLOAD_SERVICE)).enqueue(rq);
        Toast.makeText(this, name, Toast.LENGTH_SHORT).show();
    }

    static String safeName(String n) {
        StringBuilder sb = new StringBuilder();
        for (char c : n.toCharArray()) sb.append(c < 0x20 || "<>:\"/\\|?*".indexOf(c) >= 0 ? '_' : c);
        String s = sb.toString().trim();
        while (s.endsWith(".")) s = s.substring(0, s.length() - 1);
        return s.isEmpty() ? "download" : s.length() > 150 ? s.substring(0, 150) : s;
    }

    static boolean dangerous(String name) {
        int dot = name.lastIndexOf('.');
        if (dot < 0) return false;
        String e = name.substring(dot + 1).toLowerCase(Locale.ROOT);
        for (String d : DANGEROUS) if (d.equals(e)) return true;
        return false;
    }

    // ---------------------------------------------------------------- bookmarks, history
    JSONArray loadArr(String key) { try { return new JSONArray(prefs.getString(key, "[]")); } catch (Exception e) { return new JSONArray(); } }

    void addHistory(String url, String title) {
        JSONArray h = loadArr("history"), out = new JSONArray();
        JSONObject n = new JSONObject();
        try { n.put("url", url); n.put("title", title == null ? url : title); out.put(n); } catch (Exception ignored) { }
        for (int i = 0; i < h.length() && out.length() < 200; i++) if (!url.equals(h.optJSONObject(i).optString("url"))) out.put(h.opt(i));
        prefs.edit().putString("history", out.toString()).apply();
    }

    void showList(String title, String key, boolean removable) {
        JSONArray a = loadArr(key);
        String[] names = new String[a.length()];
        for (int i = 0; i < names.length; i++) names[i] = a.optJSONObject(i).optString("title");
        new AlertDialog.Builder(this).setTitle(title).setItems(names, (d, i) -> go(a.optJSONObject(i).optString("url")))
            .setNegativeButton("Close", null).show();
    }

    void toggleBookmark() {
        WebView w = cur();
        if (w == null || w.getUrl() == null || !w.getUrl().startsWith("http")) return;
        JSONArray a = loadArr("bookmarks"), out = new JSONArray();
        boolean had = false;
        for (int i = 0; i < a.length(); i++) { if (w.getUrl().equals(a.optJSONObject(i).optString("url"))) had = true; else out.put(a.opt(i)); }
        if (!had) { JSONObject o = new JSONObject(); try { o.put("url", w.getUrl()); o.put("title", w.getTitle() == null ? w.getUrl() : w.getTitle()); out.put(o); } catch (Exception ignored) { } }
        prefs.edit().putString("bookmarks", out.toString()).apply();
        Toast.makeText(this, had ? "Bookmark removed" : "Bookmarked", Toast.LENGTH_SHORT).show();
    }

    // ---------------------------------------------------------------- plugins (zip with manifest.json + .js/.css)
    File pluginsDir() { File d = new File(getFilesDir(), "plugins"); d.mkdirs(); return d; }

    static boolean validPattern(String p) {
        return p.equals("<all_urls>") || p.matches("^(\\*|https?)://(\\*|\\*\\.[A-Za-z0-9.-]+|[A-Za-z0-9.-]+)/\\S*$") && !p.contains("hb.local");
    }

    static boolean safeRel(String p) { return p != null && !p.isEmpty() && p.length() < 200 && !p.startsWith("/") && !p.startsWith("\\") && !p.contains(":") && !p.contains(".."); }

    static String slug(String n) { return n.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9]+", "-").replaceAll("^-|-$", "").replaceAll("^(.{0,40}).*$", "$1"); }

    static String read(File f) throws Exception {
        try (InputStream in = new BufferedInputStream(new FileInputStream(f)); ByteArrayOutputStream o = new ByteArrayOutputStream()) {
            byte[] b = new byte[8192]; int n;
            while ((n = in.read(b)) > 0 && o.size() < (1 << 20)) o.write(b, 0, n);
            return o.toString("UTF-8");
        }
    }

    /** Returns an error text or null; manifest of the validated folder is returned through out[0]. */
    String validatePlugin(File dir, JSONObject[] out) {
        try {
            JSONObject m = new JSONObject(read(new File(dir, "manifest.json")));
            if (m.optString("name").isEmpty() || m.optString("name").length() > 60 || m.optString("version").isEmpty()) return "name/version missing";
            JSONArray cs = m.getJSONArray("content_scripts");
            if (cs.length() == 0 || cs.length() > 10) return "content_scripts missing";
            for (int i = 0; i < cs.length(); i++) {
                JSONObject c = cs.getJSONObject(i);
                JSONArray ms = c.getJSONArray("matches");
                if (ms.length() == 0 || ms.length() > 20) return "matches missing";
                for (int k = 0; k < ms.length(); k++) if (!validPattern(ms.getString(k))) return "invalid match pattern";
                for (String kind : new String[] {"js", "css"}) {
                    JSONArray fl = c.optJSONArray(kind);
                    if (fl == null) continue;
                    for (int k = 0; k < fl.length(); k++) {
                        String rel = fl.getString(k);
                        if (!safeRel(rel) || !rel.endsWith("." + kind) || !new File(dir, rel).isFile()) return "bad file: " + rel;
                    }
                }
            }
            out[0] = m;
            return null;
        } catch (Exception e) { return "manifest is not valid"; }
    }

    boolean allSites(JSONObject m) {
        JSONArray cs = m.optJSONArray("content_scripts");
        for (int i = 0; cs != null && i < cs.length(); i++) {
            JSONArray ms = cs.optJSONObject(i).optJSONArray("matches");
            for (int k = 0; ms != null && k < ms.length(); k++) { String p = ms.optString(k); if (p.equals("<all_urls>") || p.contains("://*/")) return true; }
        }
        return false;
    }

    void pickPlugin() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("application/zip");
        startActivityForResult(i, 7);
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        if (req != 7 || res != RESULT_OK || data == null || data.getData() == null) return;
        File tmp = new File(getCacheDir(), "plugin_new");
        deleteRec(tmp);
        tmp.mkdirs();
        try (ZipInputStream z = new ZipInputStream(new BufferedInputStream(getContentResolver().openInputStream(data.getData())))) {
            ZipEntry e; int files = 0; long total = 0;
            while ((e = z.getNextEntry()) != null) {
                String n = e.getName();
                if (e.isDirectory() || !safeRel(n)) continue;
                String ext = n.contains(".") ? n.substring(n.lastIndexOf('.') + 1).toLowerCase(Locale.ROOT) : "";
                if (!java.util.Arrays.asList("js", "css", "json", "png", "jpg", "svg", "txt", "md").contains(ext)) continue;
                if (++files > 64) throw new Exception("too many files");
                File f = new File(tmp, n);
                if (!f.getCanonicalPath().startsWith(tmp.getCanonicalPath() + File.separator)) throw new Exception("bad path");
                f.getParentFile().mkdirs();
                try (OutputStream o = new FileOutputStream(f)) {
                    byte[] b = new byte[8192]; int r; long size = 0;
                    while ((r = z.read(b)) > 0) { size += r; total += r; if (size > (1 << 20) || total > (8 << 20)) throw new Exception("too big"); o.write(b, 0, r); }
                }
            }
        } catch (Exception ex) { Toast.makeText(this, "Plugin not installed: " + ex.getMessage(), Toast.LENGTH_LONG).show(); return; }
        JSONObject[] mo = new JSONObject[1];
        String err = validatePlugin(tmp, mo);
        if (err != null) { Toast.makeText(this, "Plugin not installed: " + err, Toast.LENGTH_LONG).show(); return; }
        JSONObject m = mo[0];
        StringBuilder sites = new StringBuilder();
        JSONArray cs = m.optJSONArray("content_scripts");
        int n = 0;
        for (int i = 0; i < cs.length(); i++) { JSONArray ms = cs.optJSONObject(i).optJSONArray("matches"); for (int k = 0; k < ms.length(); k++) if (n++ < 8) sites.append("  ").append(ms.optString(k)).append('\n'); }
        String id = slug(m.optString("name"));
        new AlertDialog.Builder(this).setTitle(m.optString("name") + " " + m.optString("version"))
            .setMessage("This plugin will modify these sites:\n" + sites + (allSites(m) ? "\nWARNING: it can access ALL websites!" : "") + "\n\nInstall?")
            .setPositiveButton("Install", (d, w) -> {
                File target = new File(pluginsDir(), id);
                deleteRec(target);
                if (tmp.renameTo(target)) prefs.edit().putBoolean("plugin_" + id, true).apply();
                Toast.makeText(this, "Plugin installed", Toast.LENGTH_SHORT).show();
            }).setNegativeButton("Cancel", null).show();
    }

    static void deleteRec(File f) { File[] c = f.listFiles(); if (c != null) for (File x : c) deleteRec(x); f.delete(); }

    void showPlugins() {
        File[] dirs = pluginsDir().listFiles();
        List<String> names = new ArrayList<>(); List<String> ids = new ArrayList<>();
        for (int i = 0; dirs != null && i < dirs.length; i++) {
            JSONObject[] mo = new JSONObject[1];
            if (!dirs[i].isDirectory() || validatePlugin(dirs[i], mo) != null) continue;
            ids.add(dirs[i].getName());
            names.add((prefs.getBoolean("plugin_" + dirs[i].getName(), false) ? "[on]  " : "[off] ") + mo[0].optString("name"));
        }
        if (names.isEmpty()) { Toast.makeText(this, "No plugins installed", Toast.LENGTH_SHORT).show(); return; }
        new AlertDialog.Builder(this).setTitle("Plugins (tap = on/off, long press via remove option)")
            .setItems(names.toArray(new String[0]), (d, i) -> { String k = "plugin_" + ids.get(i); prefs.edit().putBoolean(k, !prefs.getBoolean(k, false)).apply(); })
            .setNeutralButton("Remove all", (d, w) -> { deleteRec(pluginsDir()); }).setNegativeButton("Close", null).show();
    }

    void injectPlugins(WebView v, String url) {
        if (url == null || !url.startsWith("http")) return;
        File[] dirs = pluginsDir().listFiles();
        for (int i = 0; dirs != null && i < dirs.length; i++) {
            if (!prefs.getBoolean("plugin_" + dirs[i].getName(), false)) continue;
            JSONObject[] mo = new JSONObject[1];
            if (validatePlugin(dirs[i], mo) != null) continue;
            try {
                StringBuilder js = new StringBuilder("(function(){try{" + MATCHER + ";");
                JSONArray cs = mo[0].getJSONArray("content_scripts");
                for (int k = 0; k < cs.length(); k++) {
                    JSONObject c = cs.getJSONObject(k);
                    js.append("(function(){");
                    if (!c.optBoolean("all_frames", false)) js.append("if(window.top!==window.self)return;");
                    js.append("if(!").append(c.getJSONArray("matches")).append(".some(function(p){return M(p,location.href)}))return;var run=function(){");
                    JSONArray css = c.optJSONArray("css");
                    for (int j = 0; css != null && j < css.length(); j++)
                        js.append("try{var s=new CSSStyleSheet();s.replaceSync(").append(JSONObject.quote(read(new File(dirs[i], css.getString(j)))))
                          .append(");document.adoptedStyleSheets=[].concat(Array.from(document.adoptedStyleSheets),[s])}catch(e){}");
                    JSONArray jsl = c.optJSONArray("js");
                    for (int j = 0; jsl != null && j < jsl.length(); j++) js.append("try{").append(read(new File(dirs[i], jsl.getString(j)))).append("\n}catch(e){}");
                    js.append("};if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',run,{once:true});else run();})();");
                }
                js.append("}catch(e){}})();");
                v.evaluateJavascript(js.toString(), null);
            } catch (Exception ignored) { }
        }
    }

    // ---------------------------------------------------------------- menu, settings, update
    void showMenu() {
        String[] items = {"New tab", "Split view on/off", "Split 30 / 70", "Split 50 / 50", "Split 70 / 30", "Swap panes", "Split: side by side / stacked",
            "Bookmark this page", "Bookmarks", "History", "Plugins", "Add plugin (.zip)", "Find in page", "Share page", "Search engine",
            "Remove tracking from links: " + (prefs.getBoolean("cleanUrls", true) ? "on" : "off"), "Check for updates"};
        new AlertDialog.Builder(this).setItems(items, (d, i) -> {
            switch (i) {
                case 0: newTab(""); break;
                case 1: toggleSplit(); break;
                case 2: ratio = 0.3f; layoutPanes(); break;
                case 3: ratio = 0.5f; layoutPanes(); break;
                case 4: ratio = 0.7f; layoutPanes(); break;
                case 5: if (second >= 0) { int t = first; first = second; second = t; layoutPanes(); } break;
                case 6: side = !side; layoutPanes(); break;
                case 7: toggleBookmark(); break;
                case 8: showList("Bookmarks", "bookmarks", true); break;
                case 9: showList("History", "history", false); break;
                case 10: showPlugins(); break;
                case 11: pickPlugin(); break;
                case 12: findDialog(); break;
                case 13: { WebView w = cur(); if (w != null && w.getUrl() != null) { Intent s = new Intent(Intent.ACTION_SEND); s.setType("text/plain"); s.putExtra(Intent.EXTRA_TEXT, w.getUrl()); startActivity(Intent.createChooser(s, "Share")); } break; }
                case 14: engineDialog(); break;
                case 15: prefs.edit().putBoolean("cleanUrls", !prefs.getBoolean("cleanUrls", true)).apply(); break;
                case 16: updateShown = false; checkUpdate(); break;
            }
        }).show();
    }

    void findDialog() {
        EditText e = new EditText(this);
        new AlertDialog.Builder(this).setTitle("Find in page").setView(e).setPositiveButton("Find", (d, w) -> { WebView v = cur(); if (v != null) v.findAllAsync(e.getText().toString()); }).show();
    }

    void engineDialog() {
        String[] names = new String[ENGINES.length + 1];
        for (int i = 0; i < ENGINES.length; i++) names[i] = ENGINES[i][1];
        names[ENGINES.length] = "Custom (URL with %s)...";
        new AlertDialog.Builder(this).setTitle("Search engine").setItems(names, (d, i) -> {
            if (i < ENGINES.length) { prefs.edit().putString("engine", ENGINES[i][0]).apply(); return; }
            EditText e = new EditText(this);
            e.setHint("https://example.com/?q=%s");
            new AlertDialog.Builder(this).setTitle("Custom search").setView(e).setPositiveButton("Save", (d2, w) -> {
                String u = e.getText().toString().trim();
                if (validTemplate(u)) prefs.edit().putString("customUrl", u).putString("engine", "custom").apply();
                else Toast.makeText(this, "Needs an http(s) address with exactly one %s", Toast.LENGTH_LONG).show();
            }).show();
        }).show();
    }

    static int cmp(String a, String b) {
        String[] x = a.split("\\."), y = b.split("\\.");
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            int p = i < x.length ? num(x[i]) : 0, q = i < y.length ? num(y[i]) : 0;
            if (p != q) return p < q ? -1 : 1;
        }
        return 0;
    }

    static int num(String s) { try { return Integer.parseInt(s.replaceAll("\\D", "")); } catch (Exception e) { return 0; } }

    void checkUpdate() {
        new Thread(() -> {
            try {
                HttpURLConnection c = (HttpURLConnection) new URL(UPDATE_URL).openConnection();
                c.setConnectTimeout(8000); c.setReadTimeout(8000);
                try (InputStream in = c.getInputStream(); ByteArrayOutputStream o = new ByteArrayOutputStream()) {
                    byte[] b = new byte[4096]; int n;
                    while ((n = in.read(b)) > 0 && o.size() < 65536) o.write(b, 0, n);
                    JSONObject j = new JSONObject(o.toString("UTF-8"));
                    String site = j.optString("update-site", "");
                    if (!updateShown && cmp(j.optString("version", "0"), VERSION) > 0 && site.startsWith("https://") && site.length() < 2048)
                        runOnUiThread(() -> { updateShown = true; newTab(site); });
                }
            } catch (Exception ignored) { }
        }).start();
    }

    void hideKeyboard() { InputMethodManager m = (InputMethodManager) getSystemService(INPUT_METHOD_SERVICE); if (m != null) m.hideSoftInputFromWindow(urlBar.getWindowToken(), 0); }

    @Override public boolean onKeyDown(int code, KeyEvent e) {
        if (code == KeyEvent.KEYCODE_BACK) {
            WebView w = cur();
            if (w != null && w.canGoBack()) { w.goBack(); return true; }
            if (tabs.size() > 1) { closeTab(focus); return true; }
        }
        return super.onKeyDown(code, e);
    }

    @Override protected void onPause() { CookieManager.getInstance().flush(); super.onPause(); }
}
