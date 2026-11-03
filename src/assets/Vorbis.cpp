#include "cafe/assets/Vorbis.hpp"

#include <cstdlib>

#include "stb_vorbis.c"

namespace cafe {

bool decodeVorbis(const std::vector<std::uint8_t>& bytes, int& sampleRate,
                  int& channels, std::vector<std::int16_t>& samples) {
    if (bytes.empty()) return false;
    short* output = nullptr;
    const int frames = stb_vorbis_decode_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                &channels, &sampleRate, &output);
    if (frames <= 0 || !output || channels <= 0 || sampleRate <= 0) {
        std::free(output);
        return false;
    }
    samples.assign(output, output + static_cast<std::size_t>(frames * channels));
    std::free(output);
    return true;
}

} // namespace cafe
