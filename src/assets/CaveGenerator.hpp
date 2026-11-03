#pragma once

#include "cafe/assets/AssetManager.hpp"
#include <string_view>

namespace cafe {

// Legacy 500-unit cave layout, rasterized at a selectable resolution.
// Transparent pixels reveal the background underneath.
bool generateCaveImage(std::string_view source, Asset& asset,
                       int width=768,int height=768,int cornersOverride=-1);

} // namespace cafe
