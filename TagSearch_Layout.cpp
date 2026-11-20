#include "TagSearch_Layout.h"

#include <algorithm>
#include <array>

namespace tagsearch {
namespace {

void AddSlots(const RECT& area, POINT center, SIZE slot, LONG gap, std::vector<RECT>& slots) {
    const LONG width = area.right - area.left;
    const LONG height = area.bottom - area.top;
    if (slot.cx > width || slot.cy > height) return;

    const LONG columns = 1 + (width - slot.cx) / (slot.cx + gap);
    const LONG rows = 1 + (height - slot.cy) / (slot.cy + gap);
    const LONG spanX = slot.cx + (columns - 1) * (slot.cx + gap);
    const LONG spanY = slot.cy + (rows - 1) * (slot.cy + gap);
    const LONG originX = std::clamp(center.x - spanX / 2, area.left, area.right - spanX);
    const LONG originY = std::clamp(center.y - spanY / 2, area.top, area.bottom - spanY);
    for (LONG row = 0; row < rows; ++row) {
        for (LONG column = 0; column < columns; ++column) {
            const LONG x = originX + column * (slot.cx + gap);
            const LONG y = originY + row * (slot.cy + gap);
            slots.push_back({x, y, x + slot.cx, y + slot.cy});
        }
    }
}

} // namespace

std::vector<RECT> CaptionSlots(const RECT& workArea, const RECT& anchor, SIZE slot, LONG gap) {
    if (gap < 0 || slot.cx <= 0 || slot.cy <= 0 || IsRectEmpty(&workArea)) return {};

    const auto [left, top, right, bottom] = workArea;
    const POINT center{anchor.left + (anchor.right - anchor.left) / 2,
                       anchor.top + (anchor.bottom - anchor.top) / 2};
    const RECT padded{anchor.left - gap, anchor.top - gap, anchor.right + gap, anchor.bottom + gap};
    std::vector<RECT> slots;
    RECT reserved{};
    if (!IntersectRect(&reserved, &workArea, &padded)) {
        AddSlots(workArea, center, slot, gap, slots);
    } else {
        const auto [l, t, r, b] = reserved;

        // Try both ways to partition the available space. Either partition uses
        // mutually disjoint regions, even if the anchor touches a screen edge.
        const std::array<RECT, 4> horizontal{{
            {left, top, right, t}, {left, b, right, bottom}, {left, t, l, b}, {r, t, right, b},
        }};
        const std::array<RECT, 4> vertical{{
            {left, top, l, bottom}, {r, top, right, bottom}, {l, top, r, t}, {l, b, r, bottom},
        }};
        std::vector<RECT> alternative;
        for (const auto& area : horizontal) AddSlots(area, center, slot, gap, slots);
        for (const auto& area : vertical) AddSlots(area, center, slot, gap, alternative);
        if (alternative.size() > slots.size()) slots.swap(alternative);
    }

    const auto distance = [&](const RECT& rect) {
        const double dx = static_cast<double>(rect.left) + slot.cx / 2.0 - center.x;
        const double dy = static_cast<double>(rect.top) + slot.cy / 2.0 - center.y;
        return dx * dx + dy * dy;
    };
    std::stable_sort(slots.begin(), slots.end(), [&](const RECT& first, const RECT& second) {
        return distance(first) < distance(second);
    });
    return slots;
}

} // namespace tagsearch
