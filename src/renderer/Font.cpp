#include "cafe/renderer/Font.hpp"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>

namespace cafe {
extern const unsigned char kEmbeddedTtf[];
extern const unsigned int kEmbeddedTtfSize;

namespace {
int clampedSize(int pixelHeight) {
    return std::clamp(pixelHeight, 6, 96);
}

struct GlyphKey {
    int codepoint{0};
    int size{0};
    bool operator==(const GlyphKey& other) const {
        return codepoint == other.codepoint && size == other.size;
    }
};

struct GlyphKeyHash {
    std::size_t operator()(const GlyphKey& key) const {
        return static_cast<std::size_t>(key.codepoint) * 131u + static_cast<std::size_t>(key.size);
    }
};
} // namespace

struct EmbeddedFont::Impl {
    stbtt_fontinfo info{};
    bool ready{false};
    std::unordered_map<GlyphKey, GlyphBitmap, GlyphKeyHash> cache;
};

EmbeddedFont::EmbeddedFont() : impl_(std::make_unique<Impl>()) {
    (void)kEmbeddedTtfSize;
    const int offset = stbtt_GetFontOffsetForIndex(kEmbeddedTtf, 0);
    impl_->ready = offset >= 0 && stbtt_InitFont(&impl_->info, kEmbeddedTtf, offset) != 0;
}

EmbeddedFont& EmbeddedFont::instance() {
    static EmbeddedFont font;
    return font;
}

bool EmbeddedFont::ready() const { return impl_->ready; }

float EmbeddedFont::scale(float pixelHeight) const {
    if (!impl_->ready) return 0;
    return stbtt_ScaleForPixelHeight(&impl_->info, std::max(6.0f, pixelHeight));
}

float EmbeddedFont::ascent(float pixelHeight) const {
    if (!impl_->ready) return pixelHeight * 0.8f;
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&impl_->info, &ascent, &descent, &lineGap);
    return ascent * scale(pixelHeight);
}

float EmbeddedFont::kerning(int previous, int current, float pixelHeight) const {
    if (!impl_->ready || previous <= 0 || current <= 0) return 0;
    return stbtt_GetCodepointKernAdvance(&impl_->info, previous, current) * scale(pixelHeight);
}

const GlyphBitmap& EmbeddedFont::glyph(int codepoint, int pixelHeight) {
    static const GlyphBitmap empty;
    if (!impl_->ready) return empty;
    const int size = clampedSize(pixelHeight);
    const GlyphKey key{codepoint, size};
    if (auto found = impl_->cache.find(key); found != impl_->cache.end()) return found->second;

    if (impl_->cache.size() > 512) impl_->cache.clear();

    const float s = stbtt_ScaleForPixelHeight(&impl_->info, static_cast<float>(size));
    GlyphBitmap glyph;
    int advance = 0, leftSide = 0;
    stbtt_GetCodepointHMetrics(&impl_->info, codepoint, &advance, &leftSide);
    glyph.advance = advance * s;

    int w = 0, h = 0, xoff = 0, yoff = 0;
    unsigned char* bitmap = stbtt_GetCodepointBitmap(&impl_->info, s, s, codepoint, &w, &h, &xoff, &yoff);
    glyph.width = w;
    glyph.height = h;
    glyph.xoff = xoff;
    glyph.yoff = yoff;
    if (bitmap && w > 0 && h > 0) {
        glyph.coverage.assign(bitmap, bitmap + static_cast<std::size_t>(w * h));
        stbtt_FreeBitmap(bitmap, nullptr);
    }
    auto [it, _] = impl_->cache.emplace(key, std::move(glyph));
    return it->second;
}

struct FontFace::Impl {
    stbtt_fontinfo info{};
    bool ready{false};
    std::unordered_map<GlyphKey, GlyphBitmap, GlyphKeyHash> cache;
};

FontFace::FontFace(const std::vector<std::uint8_t>& bytes) : impl_(std::make_unique<Impl>()) {
    if (bytes.empty()) return;
    const int offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
    impl_->ready = offset >= 0 && stbtt_InitFont(&impl_->info, bytes.data(), offset) != 0;
}

FontFace::~FontFace() = default;
bool FontFace::ready() const { return impl_->ready; }

float FontFace::ascent(float pixelHeight) const {
    if (!impl_->ready) return pixelHeight * 0.8f;
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&impl_->info, &ascent, &descent, &lineGap);
    return ascent * stbtt_ScaleForPixelHeight(&impl_->info, std::max(6.0f, pixelHeight));
}

float FontFace::kerning(int previous, int current, float pixelHeight) const {
    if (!impl_->ready || previous <= 0 || current <= 0) return 0;
    return stbtt_GetCodepointKernAdvance(&impl_->info, previous, current) *
        stbtt_ScaleForPixelHeight(&impl_->info, std::max(6.0f, pixelHeight));
}

const GlyphBitmap& FontFace::glyph(int codepoint, int pixelHeight) {
    static const GlyphBitmap empty;
    if (!impl_->ready) return empty;
    const int size = clampedSize(pixelHeight);
    const GlyphKey key{codepoint, size};
    if (auto found = impl_->cache.find(key); found != impl_->cache.end()) return found->second;
    if (impl_->cache.size() > 512) impl_->cache.clear();

    const float scale = stbtt_ScaleForPixelHeight(&impl_->info, static_cast<float>(size));
    GlyphBitmap glyph;
    int advance = 0, leftSide = 0;
    stbtt_GetCodepointHMetrics(&impl_->info, codepoint, &advance, &leftSide);
    glyph.advance = advance * scale;
    int width = 0, height = 0, xoff = 0, yoff = 0;
    unsigned char* bitmap = stbtt_GetCodepointBitmap(&impl_->info, scale, scale,
        codepoint, &width, &height, &xoff, &yoff);
    glyph.width = width; glyph.height = height; glyph.xoff = xoff; glyph.yoff = yoff;
    if (bitmap && width > 0 && height > 0) {
        glyph.coverage.assign(bitmap, bitmap + static_cast<std::size_t>(width * height));
        stbtt_FreeBitmap(bitmap, nullptr);
    }
    return impl_->cache.emplace(key, std::move(glyph)).first->second;
}

} // namespace cafe
