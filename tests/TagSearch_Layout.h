#pragma once

#include <windows.h>

#include <random>
#include <vector>

namespace tagsearch {

// Returns disjoint slots inside the work area, in random order.
std::vector<RECT> CaptionSlots(const RECT& workArea, SIZE slot, LONG gap, std::mt19937& random);

} // namespace tagsearch
