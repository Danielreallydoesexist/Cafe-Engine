#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cafe {

struct Vec2 {
    double x{0.0};
    double y{0.0};
};

[[nodiscard]] inline Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }
[[nodiscard]] inline Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
[[nodiscard]] inline Vec2 operator*(Vec2 value, double scale) { return {value.x * scale, value.y * scale}; }

struct Vec3 {
    double x{0.0};
    double y{0.0};
    double z{0.0};
};

[[nodiscard]] inline Vec3 operator+(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
[[nodiscard]] inline Vec3 operator-(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
[[nodiscard]] inline Vec3 operator*(Vec3 value, double scale) {
    return {value.x * scale, value.y * scale, value.z * scale};
}
[[nodiscard]] inline double dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
[[nodiscard]] inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline Vec3 normalized(Vec3 value) {
    const double length = std::sqrt(dot(value, value));
    return length > 0.000001 ? value * (1.0 / length) : Vec3{};
}

struct Transform {
    Vec2 position{};
    double rotation{0.0};
    Vec2 scale{1.0, 1.0};
};

struct Transform3D {
    Vec3 position{};
    Vec3 rotation{};
    Vec3 scale{1.0, 1.0, 1.0};
};

struct Color {
    std::uint8_t r{0}, g{0}, b{0}, a{255};

    static std::optional<Color> parse(std::string_view text);
    static Color lerp(Color a, Color b, double t) {
        t = std::clamp(t, 0.0, 1.0);
        const auto mix = [t](std::uint8_t x, std::uint8_t y) {
            return static_cast<std::uint8_t>(std::lround(x + (y - x) * t));
        };
        return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
    }
};

static_assert(sizeof(Color) == 4, "Color must be packed RGBA for framebuffer copies");

enum class Display { Tv, GamePad, Both };

[[nodiscard]] inline Display parseDisplay(std::string_view value, Display fallback = Display::Both) {
    if (value == "tv" || value == "TV") return Display::Tv;
    if (value == "gamepad" || value == "pad" || value == "drc" || value == "DRC") return Display::GamePad;
    if (value == "both" || value == "all") return Display::Both;
    return fallback;
}

[[nodiscard]] inline std::string_view displayName(Display display) {
    switch (display) {
        case Display::Tv: return "tv";
        case Display::GamePad: return "gamepad";
        case Display::Both: return "both";
    }
    return "both";
}

struct Rect {
    double x{0}, y{0}, width{0}, height{0};
    [[nodiscard]] bool contains(double px, double py) const {
        return px >= x && py >= y && px <= x + width && py <= y + height;
    }
    [[nodiscard]] bool overlaps(const Rect& other) const {
        return x < other.x + other.width && x + width > other.x &&
               y < other.y + other.height && y + height > other.y;
    }
    [[nodiscard]] Vec2 center() const { return {x + width / 2.0, y + height / 2.0}; }
};

struct DualPaneLayout {
    Rect left;
    Rect divider;
    Rect right;
};

[[nodiscard]] inline DualPaneLayout splitDisplayPanes(double outputWidth,
                                                       double outputHeight,
                                                       double dividerWidth = 2.0) {
    if (outputWidth <= 0.0 || outputHeight <= 0.0) return {};
    dividerWidth = std::clamp(dividerWidth, 0.0, outputWidth);
    const double contentWidth = outputWidth - dividerWidth;
    const double leftWidth = contentWidth * 0.5;
    const double rightX = leftWidth + dividerWidth;
    return {
        {0.0, 0.0, leftWidth, outputHeight},
        {leftWidth, 0.0, dividerWidth, outputHeight},
        {rightX, 0.0, outputWidth - rightX, outputHeight},
    };
}

// Scale a virtual canvas into an output without changing its aspect ratio.
// The returned rectangle is centered in output coordinates and may leave
// letterbox or pillarbox space around the content.
[[nodiscard]] inline Rect aspectFitViewport(double virtualWidth, double virtualHeight,
                                            double outputWidth, double outputHeight) {
    if (virtualWidth <= 0.0 || virtualHeight <= 0.0 ||
        outputWidth <= 0.0 || outputHeight <= 0.0) return {};
    const double scale = std::min(outputWidth / virtualWidth, outputHeight / virtualHeight);
    const double width = virtualWidth * scale;
    const double height = virtualHeight * scale;
    return {(outputWidth - width) * 0.5, (outputHeight - height) * 0.5, width, height};
}

// Keep a scene's vertical scale and reveal more (or less) horizontal space
// to fill an output. Artwork retains its proportions; the scene stays centered.
[[nodiscard]] inline Rect expandedLogicalBounds(double virtualWidth,
                                                double virtualHeight,
                                                double outputWidth,
                                                double outputHeight) {
    if (virtualWidth <= 0.0 || virtualHeight <= 0.0 ||
        outputWidth <= 0.0 || outputHeight <= 0.0) return {};
    const double width = virtualHeight * outputWidth / outputHeight;
    return {(virtualWidth-width)*0.5,0,width,virtualHeight};
}

// Anchor a 640-style UI element to either side of an expanded logical view.
// The view is centered, so its left offset also determines the right offset.
[[nodiscard]] inline Rect anchoredScreenBounds(Rect bounds, const Rect& visible,
                                               std::string_view anchor,
                                               double fullWidth = 0.0) {
    const double edge=std::round(visible.x);
    if (anchor=="left" || anchor=="stretch" || anchor=="stretch-progress")
        bounds.x+=edge;
    else if (anchor=="right") bounds.x-=edge;
    if (anchor=="stretch") bounds.width-=2*edge;
    if (anchor=="stretch-progress" && fullWidth>0)
        bounds.width=std::max(1.0,std::round((fullWidth-2*edge)*
            std::clamp(bounds.width/fullWidth,0.0,1.0)));
    return bounds;
}

[[nodiscard]] inline std::optional<Vec2> outputToVirtual(const Rect& viewport,
                                                          double virtualWidth,
                                                          double virtualHeight,
                                                          double outputX,
                                                          double outputY) {
    if (virtualWidth <= 0.0 || virtualHeight <= 0.0 ||
        viewport.width <= 0.0 || viewport.height <= 0.0 ||
        !viewport.contains(outputX, outputY)) return std::nullopt;
    return Vec2{
        std::clamp((outputX - viewport.x) / viewport.width * virtualWidth, 0.0, virtualWidth),
        std::clamp((outputY - viewport.y) / viewport.height * virtualHeight, 0.0, virtualHeight),
    };
}

std::string trim(std::string_view value);
double number(std::string_view value, double fallback = 0.0);
bool boolean(std::string_view value, bool fallback = false);

} // namespace cafe
