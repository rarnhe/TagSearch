#pragma once

#include <windows.h>

#include <random>
#include <span>

namespace tagsearch {

// Places one rectangle per width and returns the number of distinct overlapping windows.
std::size_t LayoutCaptionWindows(const RECT& work, std::span<const LONG> widths, LONG height,
                                 std::span<RECT> bounds, std::mt19937& random);

} // namespace tagsearch
