#include "Plugins.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace hb {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

constexpr size_t kMaxFiles = 64, kMaxFileBytes = 1u << 20, kMaxTotalBytes = 8u << 20;

std::string Lower(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool Str(const json& j, const char* key, std::string& out, size_t maxLen)
{
    if (!j.contains(key) || !j[key].is_string()) return false;
    out = j[key].get<std::string>();
    return out.size() <= maxLen;
}

bool SafeRelativePath(const std::string& p)
{
    if (p.empty() || p.size() > 200) return false;
    if (p.front() == '/' || p.front() == '\\' || p.find(':') != std::string::npos) return false;
    for (unsigned char c : p) if (c < 0x20) return false;
    std::string seg;
    for (size_t i = 0; i <= p.size(); ++i)
    {
        if (i == p.size() || p[i] == '/' || p[i] == '\\')
        {
            if (seg == ".." || seg == ".") return false;
            seg.clear();
        }
        else seg += p[i];
    }
    return true;
}

bool AllowedExt(const fs::path& p)
{
    static const char* ok[] = {".js", ".css", ".json", ".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg", ".txt", ".md", ".woff2"};
    const std::string e = Lower(p.extension().string());
    return std::any_of(std::begin(ok), std::end(ok), [&](const char* x) { return e == x; });
}

std::string Slug(const std::string& name)
{
    std::string s;
    for (unsigned char c : name)
    {
        if (std::isalnum(c)) s += static_cast<char>(std::tolower(c));
        else if (!s.empty() && s.back() != '-') s += '-';
    }
    while (!s.empty() && s.back() == '-') s.pop_back();
    if (s.size() > 40) s.resize(40);
    return s;
}

std::string ReadFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// scheme://host/path parts; ok=false for anything that is not a plain http(s) web address.
struct Url { std::string scheme, host, pathQuery; bool ok = false; };

Url Split(const std::string& url)
{
    Url u;
    const auto sep = url.find("://");
    if (sep == std::string::npos) return u;
    u.scheme = Lower(url.substr(0, sep));
    if (u.scheme != "http" && u.scheme != "https") return u;
    size_t i = sep + 3;
    const size_t end = url.find_first_of("/?#", i);
    std::string auth = url.substr(i, end == std::string::npos ? std::string::npos : end - i);
    const auto at = auth.rfind('@');
    if (at != std::string::npos) auth.erase(0, at + 1);
    if (!auth.empty() && auth.front() == '[') return u;  // IPv6 literals are not matched
    const auto colon = auth.find(':');
    if (colon != std::string::npos) auth.erase(colon);
    u.host = Lower(auth);
    std::string rest = end == std::string::npos ? "" : url.substr(end);
    if (const auto hash = rest.find('#'); hash != std::string::npos) rest.erase(hash);
    if (rest.empty() || rest[0] == '?') rest.insert(0, "/");
    u.pathQuery = rest;
    u.ok = !u.host.empty();
    return u;
}

bool GlobMatch(const std::string& pat, const std::string& s)
{
    size_t p = 0, i = 0, star = std::string::npos, mark = 0;
    while (i < s.size())
    {
        if (p < pat.size() && pat[p] == '*') { star = p++; mark = i; }
        else if (p < pat.size() && pat[p] == s[i]) { ++p; ++i; }
        else if (star != std::string::npos) { p = star + 1; i = ++mark; }
        else return false;
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

}  // namespace

bool IsValidMatchPattern(const std::string& pattern)
{
    if (pattern == "<all_urls>") return true;
    if (pattern.size() > 300) return false;
    const auto sep = pattern.find("://");
    if (sep == std::string::npos) return false;
    const std::string scheme = pattern.substr(0, sep);
    if (scheme != "*" && scheme != "http" && scheme != "https") return false;
    const auto slash = pattern.find('/', sep + 3);
    if (slash == std::string::npos) return false;
    const std::string host = pattern.substr(sep + 3, slash - sep - 3);
    if (host.empty() || host == "hb.local") return false;
    if (host != "*")
    {
        const std::string h = host.rfind("*.", 0) == 0 ? host.substr(2) : host;
        if (h.empty() || h.find('*') != std::string::npos || h.find(':') != std::string::npos) return false;
        for (unsigned char c : h) if (!(std::isalnum(c) || c == '-' || c == '.')) return false;
    }
    for (unsigned char c : pattern) if (c <= 0x20 || c == 0x7f) return false;
    return true;
}

bool MatchPattern(const std::string& patternIn, const std::string& url)
{
    if (!IsValidMatchPattern(patternIn)) return false;
    const std::string pattern = patternIn == "<all_urls>" ? "*://*/*" : patternIn;
    const Url u = Split(url);
    if (!u.ok || u.host == "hb.local") return false;
    const auto sep = pattern.find("://");
    const std::string scheme = pattern.substr(0, sep);
    const auto slash = pattern.find('/', sep + 3);
    const std::string host = Lower(pattern.substr(sep + 3, slash - sep - 3));
    if (scheme != "*" && scheme != u.scheme) return false;
    if (host != "*")
    {
        if (host.rfind("*.", 0) == 0)
        {
            const std::string base = host.substr(2);
            const bool sub = u.host.size() > base.size() && u.host.compare(u.host.size() - base.size() - 1, std::string::npos, "." + base) == 0;
            if (u.host != base && !sub) return false;
        }
        else if (u.host != host) return false;
    }
    return GlobMatch(pattern.substr(slash), u.pathQuery);
}

bool ParsePlugin(const fs::path& dir, Plugin& out, std::string& error)
{
    std::error_code ec;
    const fs::path manifest = dir / "manifest.json";
    if (!fs::is_regular_file(manifest, ec) || fs::is_symlink(manifest, ec)) { error = "manifest.json missing"; return false; }
    if (fs::file_size(manifest, ec) > 100000) { error = "manifest too large"; return false; }
    const json m = json::parse(ReadFile(manifest), nullptr, false);
    if (!m.is_object()) { error = "manifest is not valid JSON"; return false; }

    Plugin p;
    p.dir = dir;
    if (!Str(m, "name", p.name, 60) || p.name.empty()) { error = "name missing or too long"; return false; }
    if (!Str(m, "version", p.version, 30) || p.version.empty()) { error = "version missing"; return false; }
    Str(m, "description", p.description, 300);
    p.id = Slug(p.name);
    if (p.id.empty()) { error = "name has no usable letters"; return false; }
    if (!m.contains("content_scripts") || !m["content_scripts"].is_array() || m["content_scripts"].empty() || m["content_scripts"].size() > 10)
    { error = "content_scripts missing (1-10 entries)"; return false; }

    for (const auto& cs : m["content_scripts"])
    {
        if (!cs.is_object() || !cs.contains("matches") || !cs["matches"].is_array() || cs["matches"].empty() || cs["matches"].size() > 20)
        { error = "matches missing"; return false; }
        PluginScript s;
        for (const auto& pat : cs["matches"])
        {
            if (!pat.is_string() || !IsValidMatchPattern(pat.get<std::string>())) { error = "invalid match pattern"; return false; }
            const std::string v = pat.get<std::string>();
            s.matches.push_back(v);
            const auto sep = v.find("://");
            if (v == "<all_urls>" || v.compare(sep + 3, 2, "*/") == 0) p.allSites = true;
        }
        for (const char* kind : {"js", "css"})
        {
            if (!cs.contains(kind)) continue;
            if (!cs[kind].is_array() || cs[kind].size() > 10) { error = "bad file list"; return false; }
            for (const auto& f : cs[kind])
            {
                const std::string rel = f.is_string() ? f.get<std::string>() : "";
                const fs::path file = dir / fs::u8path(rel);
                if (!SafeRelativePath(rel) || Lower(file.extension().string()) != (std::string(".") + kind))
                { error = "unsafe or wrong file name: " + rel; return false; }
                if (!fs::is_regular_file(file, ec) || fs::is_symlink(file, ec) || fs::file_size(file, ec) > kMaxFileBytes)
                { error = "file missing or too large: " + rel; return false; }
                (std::string(kind) == "js" ? s.js : s.css).push_back(rel);
            }
        }
        if (s.js.empty() && s.css.empty()) { error = "content script has no files"; return false; }
        s.documentStart = cs.value("run_at", "") == "document_start";
        s.allFrames = cs.contains("all_frames") && cs["all_frames"].is_boolean() && cs["all_frames"].get<bool>();
        p.scripts.push_back(std::move(s));
    }
    out = std::move(p);
    return true;
}

bool InstallPlugin(const fs::path& source, const fs::path& pluginsDir, Plugin& out, std::string& error)
{
    Plugin p;
    if (!ParsePlugin(source, p, error)) return false;
    std::error_code ec;
    size_t files = 0, total = 0;
    for (auto it = fs::recursive_directory_iterator(source, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (it->is_symlink(ec) || !it->is_regular_file(ec) || !AllowedExt(it->path())) continue;
        const auto size = it->file_size(ec);
        if (++files > kMaxFiles || size > kMaxFileBytes || (total += size) > kMaxTotalBytes) { error = "plugin is too big"; return false; }
    }
    const fs::path target = pluginsDir / p.id, staging = pluginsDir / (p.id + ".new");
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    for (auto it = fs::recursive_directory_iterator(source, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (it->is_symlink(ec) || !it->is_regular_file(ec) || !AllowedExt(it->path())) continue;
        const fs::path rel = fs::relative(it->path(), source, ec);
        fs::create_directories((staging / rel).parent_path(), ec);
        fs::copy_file(it->path(), staging / rel, fs::copy_options::overwrite_existing, ec);
        if (ec) { fs::remove_all(staging, ec); error = "copy failed"; return false; }
    }
    fs::remove_all(target, ec);
    fs::rename(staging, target, ec);
    if (ec) { error = "could not finish installation"; return false; }
    if (!ParsePlugin(target, out, error)) { fs::remove_all(target, ec); return false; }
    return true;
}

std::vector<Plugin> LoadPlugins(const fs::path& pluginsDir)
{
    std::vector<Plugin> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(pluginsDir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
    {
        Plugin p;
        std::string err;
        if (it->is_directory(ec) && !it->is_symlink(ec) && ParsePlugin(it->path(), p, err) && p.id == it->path().filename().string())
            out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(), [](const Plugin& a, const Plugin& b) { return a.name < b.name; });
    return out;
}

bool RemovePlugin(const fs::path& pluginsDir, const std::string& id)
{
    if (id.empty() || id.size() > 40 || Slug(id) != id) return false;
    std::error_code ec;
    return fs::remove_all(pluginsDir / id, ec) > 0;
}

const char* PluginMatcherJs()
{
    return "function M(p,u){if(p==='<all_urls>')p='*://*/*';var m=/^(\\*|https?):\\/\\/([^\\/]*)(\\/.*)$/.exec(p);if(!m)return false;"
           "var x;try{x=new URL(u)}catch(e){return false}var pr=x.protocol.slice(0,-1);if(pr!=='http'&&pr!=='https')return false;"
           "if(m[1]!=='*'&&m[1]!==pr)return false;var h=x.hostname.toLowerCase(),ph=m[2].toLowerCase();if(h==='hb.local')return false;"
           "if(ph!=='*'){if(ph.slice(0,2)==='*.'){var b=ph.slice(2);if(h!==b&&!h.endsWith('.'+b))return false}else if(h!==ph)return false}"
           "return new RegExp('^'+m[3].replace(/[.+?^${}()|[\\]\\\\]/g,'\\\\$&').replace(/\\*/g,'.*')+'$').test(x.pathname+x.search)}";
}

std::string BuildPluginScript(const Plugin& plugin)
{
    auto quote = [](const std::string& s) { return json(s).dump(-1, ' ', false, json::error_handler_t::replace); };
    std::string out = "(function(){try{";
    out += PluginMatcherJs();
    out += ";";
    for (const PluginScript& s : plugin.scripts)
    {
        json pats = s.matches;
        out += "(function(){";
        if (!s.allFrames) out += "if(window.top!==window.self)return;";
        out += "if(!" + pats.dump() + ".some(function(p){return M(p,location.href)}))return;";
        out += "var run=function(){";
        for (const auto& css : s.css)
            out += "try{var s=new CSSStyleSheet();s.replaceSync(" + quote(ReadFile(plugin.dir / fs::u8path(css))) +
                   ");document.adoptedStyleSheets=[].concat(Array.from(document.adoptedStyleSheets),[s])}catch(e){}";
        for (const auto& js : s.js) out += "try{" + ReadFile(plugin.dir / fs::u8path(js)) + "\n}catch(e){}";
        out += "};";
        out += s.documentStart ? "run();" : "if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',run,{once:true});else run();";
        out += "})();";
    }
    out += "}catch(e){}})();";
    return out;
}

}  // namespace hb
