#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace cafe {

struct GlyphBitmap {
    int width{0};
    int height{0};
    int xoff{0};
    int yoff{0};
    float advance{0};
    std::vector<std::uint8_t> coverage;
};

class EmbeddedFont {
public:
    static EmbeddedFont& instance();
    [[nodiscard]] bool ready() const;
    [[nodiscard]] float ascent(float pixelHeight) const;
    [[nodiscard]] float scale(float pixelHeight) const;
    [[nodiscard]] float kerning(int previous, int current, float pixelHeight) const;
    [[nodiscard]] const GlyphBitmap& glyph(int codepoint, int pixelHeight);

private:
    EmbeddedFont();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A project-provided TrueType face. The byte storage remains owned by the
// Asset that created it, while glyph bitmaps are cached per size/codepoint.
class FontFace {
public:
    explicit FontFace(const std::vector<std::uint8_t>& bytes);
    ~FontFace();
    FontFace(const FontFace&) = delete;
    FontFace& operator=(const FontFace&) = delete;
    [[nodiscard]] bool ready() const;
    [[nodiscard]] float ascent(float pixelHeight) const;
    [[nodiscard]] float kerning(int previous, int current, float pixelHeight) const;
    [[nodiscard]] const GlyphBitmap& glyph(int codepoint, int pixelHeight);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cafe
