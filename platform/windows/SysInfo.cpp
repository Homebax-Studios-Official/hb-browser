#include "SysInfo.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <iterator>
#include <sqlite3.h>

#include "WinUtil.h"

#if __has_include(<spdlog/version.h>)
#include <spdlog/version.h>
#endif

namespace hb::win {

using json = nlohmann::json;

namespace {

std::string RegString(HKEY root, const wchar_t* path, const wchar_t* name)
{
    wchar_t buf[256]{};
    DWORD size = sizeof(buf) - sizeof(wchar_t);
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    return Narrow(buf);
}

const char* LevelName(D3D_FEATURE_LEVEL l)
{
    switch (l)
    {
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    default: return "unknown";
    }
}

std::string ProbeD3D11()
{
    HMODULE m = LoadLibraryW(L"d3d11.dll");
    if (!m) return "not available";
    auto fn = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(m, "D3D11CreateDevice"));
    std::string result = "not supported";
    if (fn)
    {
        const D3D_FEATURE_LEVEL all[] = {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1,
                                         D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL got{};
        // older runtimes reject 12_x / 11_1 in the list: retry with the shorter list
        for (UINT skip : {0u, 3u})
        {
            if (SUCCEEDED(fn(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, all + skip, static_cast<UINT>(std::size(all)) - skip,
                             D3D11_SDK_VERSION, nullptr, &got, nullptr)))
            {
                result = std::string("hardware, feature level ") + LevelName(got);
                break;
            }
        }
    }
    FreeLibrary(m);
    return result;
}

std::string ProbeD3D12()
{
    HMODULE m = LoadLibraryW(L"d3d12.dll");
    if (!m) return "not available (needs Windows 10)";
    auto fn = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(m, "D3D12CreateDevice"));
    std::string result = "not supported by this GPU/driver";
    if (fn)
        for (D3D_FEATURE_LEVEL level : {D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0})
            if (SUCCEEDED(fn(nullptr, level, __uuidof(ID3D12Device), nullptr)))  // null device out: only tests support
            {
                result = std::string("supported, max feature level ") + LevelName(level);
                break;
            }
    FreeLibrary(m);
    return result;
}

json Adapters()
{
    json list = json::array();
    HMODULE m = LoadLibraryW(L"dxgi.dll");
    if (!m) return list;
    using Create = HRESULT(WINAPI*)(REFIID, void**);
    if (auto create = reinterpret_cast<Create>(GetProcAddress(m, "CreateDXGIFactory1")))
    {
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory)
        {
            IDXGIAdapter1* a = nullptr;
            for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND && i < 8; ++i)
            {
                DXGI_ADAPTER_DESC1 d{};
                if (SUCCEEDED(a->GetDesc1(&d)))
                    list.push_back({{"name", Narrow(d.Description)}, {"vramMB", static_cast<unsigned long long>(d.DedicatedVideoMemory / (1024 * 1024))},
                                    {"sharedMB", static_cast<unsigned long long>(d.SharedSystemMemory / (1024 * 1024))},
                                    {"software", (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0}});
                a->Release();
            }
            factory->Release();
        }
    }
    FreeLibrary(m);
    return list;
}

BOOL CALLBACK CountMonitor(HMONITOR, HDC, LPRECT, LPARAM lp) { ++*reinterpret_cast<int*>(lp); return TRUE; }

}  // namespace

json CollectSystemInfo(const std::filesystem::path& dataDir, const std::string& engineRuntime, const std::string& appVersion, HWND window)
{
    json j;

    // --- operating system
    json os;
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW v{sizeof(v)};
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(nt, "RtlGetVersion"))) fn(&v);
    os["name"] = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"ProductName");
    os["version"] = std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + "." + std::to_string(v.dwBuildNumber);
    os["release"] = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion");
    SYSTEM_INFO si{};
    GetNativeSystemInfo(&si);
    const char* arch = si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ? "x64" : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "ARM64"
                       : si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL ? "x86" : "other";
    os["arch"] = arch;
    j["os"] = os;

    // --- cpu, memory
    j["cpu"] = {{"name", RegString(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString")},
                {"logicalCores", static_cast<int>(si.dwNumberOfProcessors)}, {"arch", arch}};
    MEMORYSTATUSEX ms{sizeof(ms)};
    if (GlobalMemoryStatusEx(&ms))
        j["ram"] = {{"totalMB", static_cast<unsigned long long>(ms.ullTotalPhys / (1024 * 1024))},
                    {"availableMB", static_cast<unsigned long long>(ms.ullAvailPhys / (1024 * 1024))}, {"load", static_cast<int>(ms.dwMemoryLoad)}};
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    using MemFn = BOOL(WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    if (auto fn = reinterpret_cast<MemFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo")))
        if (fn(GetCurrentProcess(), reinterpret_cast<PPROCESS_MEMORY_COUNTERS>(&pmc), sizeof(pmc)))
            j["browserProcess"] = {{"workingSetMB", static_cast<unsigned long long>(pmc.WorkingSetSize / (1024 * 1024))},
                                   {"privateMB", static_cast<unsigned long long>(pmc.PrivateUsage / (1024 * 1024))}};

    // --- storage of the browser data (a USB stick is a common reason for slow starts)
    wchar_t root[MAX_PATH]{};
    if (GetVolumePathNameW(dataDir.c_str(), root, MAX_PATH))
    {
        const UINT type = GetDriveTypeW(root);
        const char* name = type == DRIVE_FIXED ? "fixed disk" : type == DRIVE_REMOVABLE ? "removable (USB/flash)" : type == DRIVE_REMOTE ? "network" : "other";
        ULARGE_INTEGER freeB{}, totalB{}, dummy{};
        GetDiskFreeSpaceExW(root, &freeB, &totalB, &dummy);
        json d = {{"drive", Narrow(root)}, {"type", name}, {"freeGB", static_cast<double>(freeB.QuadPart) / (1024.0 * 1024 * 1024)},
                  {"totalGB", static_cast<double>(totalB.QuadPart) / (1024.0 * 1024 * 1024)}};
        if (type == DRIVE_REMOVABLE || type == DRIVE_REMOTE) d["warning"] = "Browser data is on a slow drive; this slows startup and page loading.";
        j["storage"] = d;
    }

    // --- graphics
    j["gpu"] = Adapters();
    j["directx"] = {{"direct3d11", ProbeD3D11()}, {"direct3d12", ProbeD3D12()}};

    // --- screens
    int monitors = 0;
    EnumDisplayMonitors(nullptr, nullptr, CountMonitor, reinterpret_cast<LPARAM>(&monitors));
    j["display"] = {{"monitors", monitors}, {"primaryWidth", GetSystemMetrics(SM_CXSCREEN)}, {"primaryHeight", GetSystemMetrics(SM_CYSCREEN)},
                    {"dpi", window ? static_cast<int>(DpiForWindow(window)) : 96}};

    // --- SDK / build versions
    json sdk = {{"app", appVersion}, {"sqlite", sqlite3_libversion()},
                {"nlohmannJson", std::to_string(NLOHMANN_JSON_VERSION_MAJOR) + "." + std::to_string(NLOHMANN_JSON_VERSION_MINOR) + "." + std::to_string(NLOHMANN_JSON_VERSION_PATCH)},
                {"cppStandard", static_cast<long>(_MSVC_LANG)}, {"compiler", "MSVC " + std::to_string(_MSC_VER)},
                {"windowsTarget", [] { char b[16]; snprintf(b, sizeof(b), "0x%04X", static_cast<unsigned>(_WIN32_WINNT)); return std::string(b); }()}};
#ifdef SPDLOG_VER_MAJOR
    sdk["spdlog"] = std::to_string(SPDLOG_VER_MAJOR) + "." + std::to_string(SPDLOG_VER_MINOR) + "." + std::to_string(SPDLOG_VER_PATCH);
#endif
    j["sdk"] = sdk;

    // --- web engines (this is the Windows client; the other clients use their own system engine)
    j["engines"] = json::array({
        {{"name", "WebView2 (Chromium)"}, {"role", "used by this Windows client"}, {"installed", !engineRuntime.empty()},
         {"version", engineRuntime.empty() ? "not detected" : engineRuntime}},
        {{"name", "WebKitGTK"}, {"role", "used by the Linux client"}, {"installed", false}, {"version", "n/a on Windows"}},
        {{"name", "Android System WebView (Chromium)"}, {"role", "used by the Android client"}, {"installed", false}, {"version", "n/a on Windows"}}});
    return j;
}

}  // namespace hb::win
