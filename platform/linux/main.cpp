// HB Browser for Linux – GTK 3 + WebKitGTK front end. All logic that is not UI (address resolving, settings,
// bookmarks/history, plugins, download safety) comes from the shared core in src/browser/core.
#include <gtk/gtk.h>
#include <libsoup/soup.h>
#include <webkit2/webkit2.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Navigation.h"
#include "core/Plugins.h"
#include "core/Security.h"
#include "core/Settings.h"
#include "core/Storage.h"
#include "core/Util.h"

#define HB_VERSION "3.1.0"
static const char* kUpdateUrl = "https://homebax.vercel.app/browser/info.json";

using namespace hb;
using json = nlohmann::json;
namespace fs = std::filesystem;

struct Tab { WebKitWebView* view; GtkWidget* holder; GtkWidget* label; };

static struct
{
    GtkWidget *win = nullptr, *entry = nullptr, *notebook = nullptr, *paned = nullptr;
    std::vector<Tab*> tabs;
    Storage db;
    std::unique_ptr<Settings> settings;
    WebKitWebContext* ctx = nullptr;
    WebKitUserContentManager* ucm = nullptr;
    std::vector<Plugin> plugins;
    Tab *splitOwner = nullptr, *splitPartner = nullptr;
    double ratio = 0.5;
    bool side = true;
    gint64 lastCookieSync = 0;
    bool updateShown = false;
    fs::path dir;
} A;

static Tab* NewTab(const std::string& input, bool activate);

static Tab* Current()
{
    const int i = gtk_notebook_get_current_page(GTK_NOTEBOOK(A.notebook));
    GtkWidget* page = i >= 0 ? gtk_notebook_get_nth_page(GTK_NOTEBOOK(A.notebook), i) : nullptr;
    for (Tab* t : A.tabs) if (t->holder == page) return t;
    return nullptr;
}

static std::string Engine() { return A.settings->Get<std::string>("searchEngine"); }

static void Load(Tab* t, const std::string& input)
{
    if (input.empty())
    {
        const std::string tmpl = SearchUrl(Engine(), "\x01");
        const std::string html =
            "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'><title>HB Browser</title>"
            "<style>body{margin:0;height:100vh;display:grid;place-items:center;background:#14161c;color:#e8eaf0;font:16px system-ui}"
            "input{width:min(560px,86vw);padding:14px 20px;border-radius:26px;border:1px solid #333a4a;background:#1d2029;color:inherit;font-size:18px;outline:none}"
            "h1{font-weight:600;text-align:center;margin:0 0 22px}</style><div><h1>HB Browser</h1><input id=q autofocus placeholder='Search or enter address'></div>"
            "<script>const T=" + json(tmpl).dump() + ";q.onkeydown=e=>{if(e.key==='Enter'&&q.value.trim()){const v=q.value.trim();"
            "location.href=/^[\\w+.-]+:\\/\\//.test(v)?v:T.replace('\\u0001',encodeURIComponent(v))}}</script>";
        webkit_web_view_load_html(t->view, html.c_str(), "about:blank");
        return;
    }
    webkit_web_view_load_uri(t->view, ResolveInput(input, Engine()).c_str());
}

// ----------------------------------------------------------------------------- split view
static void ApplyRatio()
{
    if (!A.paned) return;
    const bool h = gtk_orientable_get_orientation(GTK_ORIENTABLE(A.paned)) == GTK_ORIENTATION_HORIZONTAL;
    const int total = h ? gtk_widget_get_allocated_width(A.paned) : gtk_widget_get_allocated_height(A.paned);
    gtk_paned_set_position(GTK_PANED(A.paned), static_cast<int>(total * std::clamp(A.ratio, 0.2, 0.8)));
}

static void EndSplit()
{
    if (!A.paned) return;
    Tab *o = A.splitOwner, *p = A.splitPartner;
    g_object_ref(o->view);
    g_object_ref(p->view);
    gtk_container_remove(GTK_CONTAINER(A.paned), GTK_WIDGET(o->view));
    gtk_container_remove(GTK_CONTAINER(A.paned), GTK_WIDGET(p->view));
    gtk_container_remove(GTK_CONTAINER(o->holder), A.paned);
    gtk_box_pack_start(GTK_BOX(o->holder), GTK_WIDGET(o->view), TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(p->holder), GTK_WIDGET(p->view), TRUE, TRUE, 0);
    g_object_unref(o->view);
    g_object_unref(p->view);
    A.paned = nullptr;
    A.splitOwner = A.splitPartner = nullptr;
}

static gboolean RatioIdle(gpointer) { ApplyRatio(); return G_SOURCE_REMOVE; }

static void StartSplit()
{
    Tab* o = Current();
    if (!o) return;
    Tab* p = nullptr;
    for (size_t i = 0; i < A.tabs.size() && !p; ++i)
        if (A.tabs[i] == o && i + 1 < A.tabs.size()) p = A.tabs[i + 1];
    if (!p) p = NewTab("", false);
    g_object_ref(o->view);
    g_object_ref(p->view);
    gtk_container_remove(GTK_CONTAINER(o->holder), GTK_WIDGET(o->view));
    gtk_container_remove(GTK_CONTAINER(p->holder), GTK_WIDGET(p->view));
    A.paned = gtk_paned_new(A.side ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL);
    gtk_paned_pack1(GTK_PANED(A.paned), GTK_WIDGET(o->view), TRUE, FALSE);
    gtk_paned_pack2(GTK_PANED(A.paned), GTK_WIDGET(p->view), TRUE, FALSE);
    gtk_box_pack_start(GTK_BOX(o->holder), A.paned, TRUE, TRUE, 0);
    gtk_widget_show_all(o->holder);
    g_object_unref(o->view);
    g_object_unref(p->view);
    A.splitOwner = o;
    A.splitPartner = p;
    g_idle_add(RatioIdle, nullptr);
}

// ----------------------------------------------------------------------------- plugins
static void ReloadPlugins()
{
    A.plugins = LoadPlugins(A.dir / "plugins");
    webkit_user_content_manager_remove_all_scripts(A.ucm);
    const json state = A.settings->Get<json>("plugins");
    for (const Plugin& p : A.plugins)
    {
        if (!(state.is_object() && state.contains(p.id) && state[p.id].is_boolean() && state[p.id].get<bool>())) continue;
        const std::string src = BuildPluginScript(p);  // matches the address itself, never runs on internal pages
        WebKitUserScript* s = webkit_user_script_new(src.c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                                     WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
        webkit_user_content_manager_add_script(A.ucm, s);
        webkit_user_script_unref(s);
    }
}

static void InstallPluginDialog()
{
    GtkWidget* fc = gtk_file_chooser_dialog_new("Plugin folder", GTK_WINDOW(A.win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                "_Cancel", GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_ACCEPT, nullptr);
    if (gtk_dialog_run(GTK_DIALOG(fc)) != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(fc); return; }
    char* chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(fc));
    gtk_widget_destroy(fc);
    std::error_code ec;
    fs::create_directories(A.dir / "plugins", ec);
    Plugin p;
    std::string err;
    const bool ok = chosen && InstallPlugin(fs::path(chosen), A.dir / "plugins", p, err);
    g_free(chosen);
    GtkWidget* d;
    if (!ok)
    {
        d = gtk_message_dialog_new(GTK_WINDOW(A.win), GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Plugin not installed: %s", err.c_str());
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }
    std::string sites;
    int n = 0;
    for (const auto& s : p.scripts) for (const auto& m : s.matches) if (n++ < 8) sites += "  " + m + "\n";
    d = gtk_message_dialog_new(GTK_WINDOW(A.win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_YES_NO,
                               "%s %s\n\nThis plugin will modify these sites:\n%s%s\nInstall?", p.name.c_str(), p.version.c_str(),
                               sites.c_str(), p.allSites ? "\nWARNING: it can access ALL websites!\n" : "");
    const int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (r != GTK_RESPONSE_YES) { RemovePlugin(A.dir / "plugins", p.id); return; }
    json state = A.settings->Get<json>("plugins");
    if (!state.is_object()) state = json::object();
    state[p.id] = true;
    A.settings->Set("plugins", state);
    A.settings->Save();
    ReloadPlugins();
}

// ----------------------------------------------------------------------------- web view signals
static void SyncCookies(WebKitCookieManager* cm);

static void OnCookies(GObject* src, GAsyncResult* res, gpointer)
{
    auto* cm = WEBKIT_COOKIE_MANAGER(src);
    GList* list = webkit_cookie_manager_get_all_cookies_finish(cm, res, nullptr);
    for (GList* i = list; i; i = i->next)
    {
        auto* c = static_cast<SoupCookie*>(i->data);
        if (soup_cookie_get_expires(c) == nullptr && soup_cookie_get_secure(c))  // session cookie of a secure site
        {
            SoupCookie* copy = soup_cookie_copy(c);
            soup_cookie_set_max_age(copy, 30 * 86400);
            webkit_cookie_manager_add_cookie(cm, copy, nullptr, nullptr, nullptr);
            soup_cookie_free(copy);
        }
    }
    g_list_free_full(list, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
}

static void SyncCookies(WebKitCookieManager* cm)
{
    const gint64 now = g_get_monotonic_time() / 1000000;
    if (!A.settings->Get<bool>("stayLoggedIn") || now - A.lastCookieSync < 20) return;
    A.lastCookieSync = now;
    webkit_cookie_manager_get_all_cookies(cm, nullptr, OnCookies, nullptr);
}

static Tab* TabOf(WebKitWebView* v)
{
    for (Tab* t : A.tabs) if (t->view == v) return t;
    return nullptr;
}

static void OnTitle(WebKitWebView* v, GParamSpec*, gpointer)
{
    Tab* t = TabOf(v);
    const char* title = webkit_web_view_get_title(v);
    if (t && title && *title) gtk_label_set_text(GTK_LABEL(t->label), title);
    const char* uri = webkit_web_view_get_uri(v);
    if (uri && title && StartsWith(uri, "http") && !webkit_web_view_is_ephemeral(v)) A.db.UpdateTitle(uri, title);
}

static void OnUri(WebKitWebView* v, GParamSpec*, gpointer)
{
    Tab* t = TabOf(v);
    const char* uri = webkit_web_view_get_uri(v);
    if (t == Current() && uri && std::string(uri) != "about:blank") gtk_entry_set_text(GTK_ENTRY(A.entry), uri);
}

static void OnLoad(WebKitWebView* v, WebKitLoadEvent e, gpointer)
{
    if (e != WEBKIT_LOAD_FINISHED) return;
    const char* uri = webkit_web_view_get_uri(v);
    if (uri && StartsWith(uri, "http"))
    {
        const char* title = webkit_web_view_get_title(v);
        A.db.AddVisit(uri, title ? title : "");
        SyncCookies(webkit_website_data_manager_get_cookie_manager(webkit_web_context_get_website_data_manager(A.ctx)));
    }
}

static gboolean OnDecide(WebKitWebView* v, WebKitPolicyDecision* d, WebKitPolicyDecisionType type, gpointer)
{
    if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) return FALSE;
    WebKitNavigationAction* na = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(d));
    const std::string uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(na));
    const bool user = webkit_navigation_action_is_user_gesture(na);
    if (!IsAllowedNavigation(uri, user)) { webkit_policy_decision_ignore(d); return TRUE; }
    if (user && A.settings->Get<bool>("cleanUrls"))
    {
        const std::string cleaned = StripTrackingParams(uri);
        if (cleaned != uri) { webkit_policy_decision_ignore(d); webkit_web_view_load_uri(v, cleaned.c_str()); return TRUE; }
    }
    return FALSE;
}

static GtkWidget* OnCreate(WebKitWebView*, WebKitNavigationAction* na, gpointer)
{
    const std::string uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(na));
    if (IsAllowedNavigation(uri, true)) NewTab(uri, true);
    return nullptr;  // we opened our own tab
}

static gboolean OnPermission(WebKitWebView* v, WebKitPermissionRequest* r, gpointer)
{
    const char* uri = webkit_web_view_get_uri(v);
    GtkWidget* d = gtk_message_dialog_new(GTK_WINDOW(A.win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                                          "%s\nwants a permission (camera, location, notifications ...).\nAllow this time?", uri ? uri : "");
    const int resp = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (resp == GTK_RESPONSE_YES) webkit_permission_request_allow(r); else webkit_permission_request_deny(r);
    return TRUE;
}

static gboolean OnDestination(WebKitDownload* d, const gchar* suggested, gpointer)
{
    const std::string name = SanitizeFileName(suggested ? suggested : "download");
    const char* reqUri = webkit_uri_request_get_uri(webkit_download_get_request(d));
    if (A.settings->Get<bool>("blockInsecureDownloads") && reqUri && SchemeOf(reqUri) == "http" && IsDangerousFileName(name))
    {
        webkit_download_cancel(d);
        return TRUE;
    }
    const char* dl = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    fs::path dir = dl ? dl : g_get_home_dir(), path = dir / name;
    for (int i = 1; fs::exists(path) && i < 1000; ++i)
        path = dir / (fs::path(name).stem().string() + " (" + std::to_string(i) + ")" + fs::path(name).extension().string());
    char* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
    if (uri) { webkit_download_set_destination(d, uri); g_free(uri); }
    return TRUE;
}

static void OnDownloadStarted(WebKitWebContext*, WebKitDownload* d, gpointer)
{
    g_signal_connect(d, "decide-destination", G_CALLBACK(OnDestination), nullptr);
}

// ----------------------------------------------------------------------------- tabs
static void CloseTab(Tab* t)
{
    EndSplit();
    const int idx = gtk_notebook_page_num(GTK_NOTEBOOK(A.notebook), t->holder);
    A.tabs.erase(std::remove(A.tabs.begin(), A.tabs.end(), t), A.tabs.end());
    if (idx >= 0) gtk_notebook_remove_page(GTK_NOTEBOOK(A.notebook), idx);
    delete t;
    if (A.tabs.empty()) gtk_main_quit();
}

static Tab* NewTab(const std::string& input, bool activate)
{
    auto* t = new Tab;
    t->view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", A.ctx, "user-content-manager", A.ucm, nullptr));
    t->holder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(t->holder), GTK_WIDGET(t->view), TRUE, TRUE, 0);
    t->label = gtk_label_new("New tab");
    gtk_label_set_ellipsize(GTK_LABEL(t->label), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(t->label), 18);
    GtkWidget* close = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(head), t->label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(head), close, FALSE, FALSE, 0);
    gtk_widget_show_all(head);
    g_signal_connect_swapped(close, "clicked", G_CALLBACK(+[](Tab* tab) { CloseTab(tab); }), t);

    WebKitSettings* ws = webkit_web_view_get_settings(t->view);
    webkit_settings_set_enable_developer_extras(ws, TRUE);
    webkit_settings_set_enable_write_console_messages_to_stdout(ws, FALSE);
    webkit_settings_set_enable_java(ws, FALSE);
    webkit_settings_set_allow_file_access_from_file_urls(ws, FALSE);
    webkit_settings_set_allow_universal_access_from_file_urls(ws, FALSE);
    webkit_settings_set_javascript_can_open_windows_automatically(ws, FALSE);

    g_signal_connect(t->view, "notify::title", G_CALLBACK(OnTitle), nullptr);
    g_signal_connect(t->view, "notify::uri", G_CALLBACK(OnUri), nullptr);
    g_signal_connect(t->view, "load-changed", G_CALLBACK(OnLoad), nullptr);
    g_signal_connect(t->view, "decide-policy", G_CALLBACK(OnDecide), nullptr);
    g_signal_connect(t->view, "create", G_CALLBACK(OnCreate), nullptr);
    g_signal_connect(t->view, "permission-request", G_CALLBACK(OnPermission), nullptr);

    A.tabs.push_back(t);
    const int page = gtk_notebook_append_page(GTK_NOTEBOOK(A.notebook), t->holder, head);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(A.notebook), t->holder, TRUE);
    gtk_widget_show_all(t->holder);
    Load(t, input);
    if (activate) gtk_notebook_set_current_page(GTK_NOTEBOOK(A.notebook), page);
    return t;
}

// ----------------------------------------------------------------------------- update check (libsoup 3)
static void OnInfo(GObject* src, GAsyncResult* res, gpointer)
{
    GBytes* bytes = soup_session_send_and_read_finish(SOUP_SESSION(src), res, nullptr);
    if (!bytes) return;
    gsize len = 0;
    const char* data = static_cast<const char*>(g_bytes_get_data(bytes, &len));
    const json info = json::parse(std::string(data, std::min<gsize>(len, 65536)), nullptr, false);
    g_bytes_unref(bytes);
    auto str = [&info](const char* k) { return info.is_object() && info.contains(k) && info[k].is_string() ? info[k].get<std::string>() : std::string(); };
    const std::string site = str("update-site");
    if (!A.updateShown && CompareVersions(str("version"), HB_VERSION) > 0 && SchemeOf(site) == "https" && IsSafeTabUrl(site))
    {
        A.updateShown = true;
        NewTab(site, true);
    }
}

static void CheckUpdate()
{
    if (!A.settings->Get<bool>("checkUpdates")) return;
    SoupSession* s = soup_session_new();
    SoupMessage* m = soup_message_new("GET", kUpdateUrl);
    if (m) soup_session_send_and_read_async(s, m, G_PRIORITY_LOW, nullptr, OnInfo, nullptr);
}

// ----------------------------------------------------------------------------- UI
static void AddItem(GtkWidget* menu, const char* label, std::function<void()> fn)
{
    GtkWidget* item = gtk_menu_item_new_with_label(label);
    auto* f = new std::function<void()>(std::move(fn));
    g_signal_connect(item, "activate", G_CALLBACK(+[](GtkMenuItem*, gpointer p) { (*static_cast<std::function<void()>*>(p))(); }), f);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static void ShowDevTools()
{
    if (Tab* t = Current())
    {
        WebKitWebInspector* i = webkit_web_view_get_inspector(t->view);
        webkit_web_inspector_show(i);
        webkit_web_inspector_attach(i);  // docked inside the window
    }
}

static gboolean OnKey(GtkWidget*, GdkEventKey* e, gpointer)
{
    const bool ctrl = e->state & GDK_CONTROL_MASK;
    Tab* t = Current();
    if (e->keyval == GDK_KEY_F12) { ShowDevTools(); return TRUE; }
    if (!ctrl) return FALSE;
    switch (e->keyval)
    {
    case GDK_KEY_t: NewTab("", true); gtk_widget_grab_focus(A.entry); return TRUE;
    case GDK_KEY_w: if (t) CloseTab(t); return TRUE;
    case GDK_KEY_l: gtk_widget_grab_focus(A.entry); return TRUE;
    case GDK_KEY_r: if (t) webkit_web_view_reload(t->view); return TRUE;
    case GDK_KEY_d:
        if (t && webkit_web_view_get_uri(t->view))
        {
            const std::string u = webkit_web_view_get_uri(t->view);
            if (!A.db.RemoveBookmarkByUrl(u)) A.db.AddBookmark(u, webkit_web_view_get_title(t->view) ? webkit_web_view_get_title(t->view) : u);
        }
        return TRUE;
    case GDK_KEY_E: case GDK_KEY_e: if (e->state & GDK_SHIFT_MASK) { if (A.paned) EndSplit(); else StartSplit(); return TRUE; } return FALSE;
    }
    return FALSE;
}

static void BuildUi()
{
    A.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(A.win), "HB Browser " HB_VERSION);
    gtk_window_set_default_size(GTK_WINDOW(A.win), 1280, 800);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(bar), 4);
    auto mk = [&](const char* icon, std::function<void(Tab*)> fn)
    {
        GtkWidget* b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_SMALL_TOOLBAR);
        auto* f = new std::function<void(Tab*)>(std::move(fn));
        g_signal_connect(b, "clicked", G_CALLBACK(+[](GtkButton*, gpointer p) { if (Tab* t = Current()) (*static_cast<std::function<void(Tab*)>*>(p))(t); }), f);
        gtk_box_pack_start(GTK_BOX(bar), b, FALSE, FALSE, 0);
    };
    mk("go-previous-symbolic", [](Tab* t) { webkit_web_view_go_back(t->view); });
    mk("go-next-symbolic", [](Tab* t) { webkit_web_view_go_forward(t->view); });
    mk("view-refresh-symbolic", [](Tab* t) { webkit_web_view_reload(t->view); });
    A.entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(A.entry), "Search or enter address");
    g_signal_connect(A.entry, "activate", G_CALLBACK(+[](GtkEntry* e, gpointer) { if (Tab* t = Current()) Load(t, gtk_entry_get_text(e)); }), nullptr);
    gtk_box_pack_start(GTK_BOX(bar), A.entry, TRUE, TRUE, 0);

    GtkWidget* menu = gtk_menu_new();
    AddItem(menu, "New tab (Ctrl+T)", [] { NewTab("", true); });
    AddItem(menu, "Split view on/off (Ctrl+Shift+E)", [] { if (A.paned) EndSplit(); else StartSplit(); });
    for (double r : {0.3, 0.5, 0.7})
        AddItem(menu, r == 0.3 ? "Split 30 / 70" : r == 0.5 ? "Split 50 / 50" : "Split 70 / 30", [r] { A.ratio = r; ApplyRatio(); });
    AddItem(menu, "Split: side by side / stacked", [] {
        if (!A.paned) return;
        A.side = !A.side;
        gtk_orientable_set_orientation(GTK_ORIENTABLE(A.paned), A.side ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL);
        ApplyRatio();
    });
    AddItem(menu, "Add plugin (folder)...", InstallPluginDialog);
    AddItem(menu, "Developer tools (F12)", ShowDevTools);
    AddItem(menu, "Bookmarks", [] {
        json b = A.db.Bookmarks();
        std::string html = "<meta charset=utf-8><title>Bookmarks</title><body style='font:16px system-ui;background:#14161c;color:#e8eaf0;padding:24px'><h2>Bookmarks</h2>";
        auto esc = [](const std::string& s) { std::string o; for (char c : s) o += c == '<' ? "&lt;" : c == '>' ? "&gt;" : c == '&' ? "&amp;" : c == '"' ? "&quot;" : std::string(1, c); return o; };
        for (const auto& x : b) html += "<p><a style='color:#8fb0ff' href=\"" + esc(x.value("url", "")) + "\">" + esc(x.value("title", x.value("url", ""))) + "</a></p>";
        Tab* t = NewTab("about:blank", true);
        webkit_web_view_load_html(t->view, html.c_str(), "about:blank");
    });
    AddItem(menu, "Quit", [] { gtk_main_quit(); });
    gtk_widget_show_all(menu);
    GtkWidget* mb = gtk_menu_button_new();
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(mb), menu);
    gtk_box_pack_start(GTK_BOX(bar), mb, FALSE, FALSE, 0);

    A.notebook = gtk_notebook_new();
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(A.notebook), TRUE);
    g_signal_connect(A.notebook, "switch-page", G_CALLBACK(+[](GtkNotebook*, GtkWidget*, guint, gpointer) { EndSplit(); }), nullptr);
    g_signal_connect(A.notebook, "switch-page", G_CALLBACK(+[](GtkNotebook*, GtkWidget* page, guint, gpointer)
    {
        for (Tab* t : A.tabs) if (t->holder == page) { const char* u = webkit_web_view_get_uri(t->view); gtk_entry_set_text(GTK_ENTRY(A.entry), u && std::string(u) != "about:blank" ? u : ""); }
    }), nullptr);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), A.notebook, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(A.win), box);
    g_signal_connect(A.win, "destroy", G_CALLBACK(gtk_main_quit), nullptr);
    g_signal_connect(A.win, "key-press-event", G_CALLBACK(OnKey), nullptr);
}

int main(int argc, char** argv)
{
    gtk_init(&argc, &argv);
    A.dir = fs::path(g_get_user_data_dir()) / "hb-browser";
    std::error_code ec;
    fs::create_directories(A.dir, ec);
    fs::permissions(A.dir, fs::perms::owner_all, fs::perm_options::replace, ec);  // private to this user
    A.settings = std::make_unique<Settings>(A.dir / "settings.json");
    A.settings->Load();
    SetCustomSearchEngines(Settings::CustomEngines(A.settings->Get<json>("customSearchEngines")));
    A.db.Open(A.dir / "browser.db");

    const fs::path cache = fs::path(g_get_user_cache_dir()) / "hb-browser";
    WebKitWebsiteDataManager* dm = webkit_website_data_manager_new("base-data-directory", (A.dir / "web").c_str(), "base-cache-directory", cache.c_str(), nullptr);
    webkit_website_data_manager_set_itp_enabled(dm, TRUE);
    WebKitCookieManager* cm = webkit_website_data_manager_get_cookie_manager(dm);
    webkit_cookie_manager_set_persistent_storage(cm, (A.dir / "cookies.sqlite").c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    A.ctx = webkit_web_context_new_with_website_data_manager(dm);
    A.ucm = webkit_user_content_manager_new();
    g_signal_connect(A.ctx, "download-started", G_CALLBACK(OnDownloadStarted), nullptr);
    ReloadPlugins();

    BuildUi();
    gtk_widget_show_all(A.win);
    if (argc > 1) for (int i = 1; i < argc && i < 20; ++i) NewTab(argv[i], true);
    else NewTab("", true);
    CheckUpdate();
    gtk_main();
    A.settings->Save();
    return 0;
}
