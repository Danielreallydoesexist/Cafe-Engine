#pragma once

#include "cafe/assets/AssetManager.hpp"
#include <string_view>

namespace cafe {

// Generates a bounded, deterministic image from a procedural: source string.
// The caller owns the resulting pixels and may cache them as an ordinary image.
bool generateProceduralImage(std::string_view source, Asset& asset);
std::vector<MaskEdgePoint> extractMaskEdgePoints(const Asset& asset,
                                                int spacing,double threshold);

} // namespace cafe
