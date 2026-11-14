// HB plugins: a small, safe subset of the Chromium "content script" extension format that works the same on
// Windows, Linux and Android. A plugin is a folder with manifest.json + .js/.css files that are injected into
// the web pages its "matches" patterns name. No native code, no chrome.* APIs, never runs on internal pages.
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace hb {

struct PluginScript
{
    std::vector<std::string> matches, js, css;
    bool documentStart = false;
    bool allFrames = false;
};

struct Plugin
{
    std::string id, name, version, description;
    std::vector<PluginScript> scripts;
    std::filesystem::path dir;
    bool allSites = false;  // needs an explicit confirmation from the user
};

bool IsValidMatchPattern(const std::string& pattern);
bool MatchPattern(const std::string& pattern, const std::string& url);

// Validates a plugin folder (manifest, paths, sizes). On failure `error` holds a short English reason.
bool ParsePlugin(const std::filesystem::path& dir, Plugin& out, std::string& error);
// Copies a validated plugin (only allowed file types) to pluginsDir/<id>; replaces an older version.
bool InstallPlugin(const std::filesystem::path& source, const std::filesystem::path& pluginsDir, Plugin& out, std::string& error);
std::vector<Plugin> LoadPlugins(const std::filesystem::path& pluginsDir);
bool RemovePlugin(const std::filesystem::path& pluginsDir, const std::string& id);

// One self-contained script (matcher included) that is safe to inject into every page at document start.
std::string BuildPluginScript(const Plugin& plugin);
// The JavaScript matcher alone (kept identical to MatchPattern, tested against the same vectors).
const char* PluginMatcherJs();

}  // namespace hb
