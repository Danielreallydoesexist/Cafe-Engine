#pragma once

#include <cstdint>
#include <vector>

namespace cafe {

bool decodeVorbis(const std::vector<std::uint8_t>& bytes, int& sampleRate,
                  int& channels, std::vector<std::int16_t>& samples);

} // namespace cafe
