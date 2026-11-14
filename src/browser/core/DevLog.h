// Developer log: timing of every startup step and page load, plus plain-language hints about what slows things down.
// Platform neutral (no OS calls) so Windows, Linux and Android front ends can share the analysis.
#pragma once
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace hb {

struct DevEntry
{
    long long atMs = 0;     // since process start
    long long deltaMs = 0;  // since the previous mark
    std::string text;
};

class DevLog
{
public:
    // offsetMs = time that already passed between process creation and this call.
    void Open(const std::filesystem::path& file, long long offsetMs);
    void Mark(const std::string& step);               // startup step: logged with total and delta time
    void Record(long long atMs, const std::string& step);  // same with an explicit time (used by Mark and tests)
    void Info(const std::string& text);               // free text line
    void Timing(const std::string& what, long long ms);  // page load / resource timing line
    bool enabled() const { return enabled_; }
    void SetEnabled(bool on) { enabled_ = on; }
    const std::vector<DevEntry>& Entries() const { return marks_; }
    long long NowMs() const;

    // Human readable explanations for slow steps, derived from the recorded marks.
    std::vector<std::string> Hints() const;
    // Writes the hints to the file (call once, after the first page loaded).
    void WriteHints();

private:
    void Write(const std::string& line);
    long long Find(const std::string& step) const;  // -1 when not recorded
    std::ofstream out_;
    std::vector<DevEntry> marks_;
    long long offsetMs_ = 0;
    long long startTick_ = 0;
    long long lastMs_ = 0;
    bool enabled_ = true;
};

}  // namespace hb
