// Split view geometry shared by every platform front end (header only, no OS types).
#pragma once
#include <algorithm>

namespace hb {

struct SplitRect { int x = 0, y = 0, w = 0, h = 0; };
struct SplitResult { SplitRect first, second; };

inline double ClampSplitRatio(double r) { return std::clamp(r, 0.2, 0.8); }

// ratio = share of the first pane. sideBySide: first pane left, second right; otherwise first on top.
inline SplitResult ComputeSplit(SplitRect t, double ratio, bool sideBySide, int gap)
{
    ratio = ClampSplitRatio(ratio);
    gap = std::max(0, gap);
    SplitResult r;
    if (sideBySide)
    {
        const int usable = std::max(0, t.w - gap);
        const int a = static_cast<int>(usable * ratio + 0.5);
        r.first = {t.x, t.y, a, t.h};
        r.second = {t.x + a + gap, t.y, usable - a, t.h};
    }
    else
    {
        const int usable = std::max(0, t.h - gap);
        const int a = static_cast<int>(usable * ratio + 0.5);
        r.first = {t.x, t.y, t.w, a};
        r.second = {t.x, t.y + a + gap, t.w, usable - a};
    }
    return r;
}

}  // namespace hb
