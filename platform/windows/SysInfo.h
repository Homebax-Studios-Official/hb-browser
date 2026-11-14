// System information for the Dev panel and dev.log: CPU, RAM, GPU, DirectX support, SDK versions, web engines.
#pragma once
#include <windows.h>

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace hb::win {

// engineRuntime = version string of the WebView2 runtime in use (empty = not detected).
nlohmann::json CollectSystemInfo(const std::filesystem::path& dataDir, const std::string& engineRuntime, const std::string& appVersion, HWND window);

}  // namespace hb::win
