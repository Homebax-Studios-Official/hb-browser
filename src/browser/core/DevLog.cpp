#include "DevLog.h"

#include <chrono>

namespace hb {

namespace {
long long SteadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

long long DevLog::NowMs() const { return offsetMs_ + (SteadyMs() - startTick_); }

void DevLog::Open(const std::filesystem::path& file, long long offsetMs)
{
    offsetMs_ = offsetMs < 0 ? 0 : offsetMs;
    startTick_ = SteadyMs();
    lastMs_ = 0;
    marks_.clear();
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    // keep one previous run for comparison
    if (std::filesystem::exists(file, ec))
    {
        auto prev = file;
        prev.replace_filename("dev.prev.log");
        std::filesystem::copy_file(file, prev, std::filesystem::copy_options::overwrite_existing, ec);
    }
    out_.open(file, std::ios::out | std::ios::trunc);
    Write("# HB Browser dev.log - times are milliseconds since the process was created");
}

void DevLog::Write(const std::string& line)
{
    if (!enabled_ || !out_.is_open()) return;
    out_ << line << '\n';
    out_.flush();
}

void DevLog::Mark(const std::string& step) { Record(NowMs(), step); }

void DevLog::Record(long long now, const std::string& step)
{
    DevEntry e{now, now - lastMs_, step};
    lastMs_ = now;
    marks_.push_back(e);
    Write("[" + std::to_string(now) + " ms] STEP " + step + "  (+" + std::to_string(e.deltaMs) + " ms)");
}

void DevLog::Info(const std::string& text) { Write("[" + std::to_string(NowMs()) + " ms] INFO " + text); }

void DevLog::Timing(const std::string& what, long long ms)
{
    Write("[" + std::to_string(NowMs()) + " ms] LOAD " + what + "  took " + std::to_string(ms) + " ms");
}

long long DevLog::Find(const std::string& step) const
{
    for (const auto& m : marks_) if (m.text == step) return m.atMs;
    return -1;
}

std::vector<std::string> DevLog::Hints() const
{
    std::vector<std::string> h;
    const long long ctl = Find("controller start"), req = Find("engine requested"), eng = Find("engine ready"),
                    ui = Find("browser UI ready"), page = Find("first page loaded");
    if (ctl > 900)
        h.push_back("Process start to controller took " + std::to_string(ctl) + " ms: antivirus scanning the program, a slow or USB drive, or a busy computer.");
    if (req >= 0 && eng >= 0 && eng - req > 1500)
        h.push_back("Web engine start took " + std::to_string(eng - req) + " ms: cold start of the engine runtime (first launch after boot), antivirus, little free RAM, or a slow drive.");
    if (eng >= 0 && ui >= 0 && ui - eng > 800)
        h.push_back("Browser UI took " + std::to_string(ui - eng) + " ms after the engine was ready: creating the UI view / loading shell page is slow (disk or CPU).");
    if (ui >= 0 && page >= 0 && page - ui > 1000)
        h.push_back("First page took " + std::to_string(page - ui) + " ms after the UI: the page or its resources load slowly.");
    if (page >= 0 && page > 4000) h.push_back("Total startup is " + std::to_string(page) + " ms (over 4 s).");
    if (h.empty() && page >= 0) h.push_back("No slow startup step found (total " + std::to_string(page) + " ms).");
    return h;
}

void DevLog::WriteHints()
{
    Write("# ---- analysis ----");
    for (const auto& s : Hints()) Write("HINT " + s);
}

}  // namespace hb
