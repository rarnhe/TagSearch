#pragma once

#include <windows.h>

#include <vector>

namespace tagsearch {

// Returns disjoint slots inside the work area, nearest the anchor first.
std::vector<RECT> CaptionSlots(const RECT& workArea, const RECT& anchor, SIZE slot, LONG gap);

} // namespace tagsearch
