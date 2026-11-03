#include "cafe/renderer/Renderer.hpp"
#include "cafe/renderer/Font.hpp"
#include "ColorFilter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string_view>
#include <cstdlib>

namespace cafe {
namespace {

Color sampleStops(const std::vector<ColorStop>& stops, double t) {
    t = std::clamp(t, 0.0, 1.0);
    if (stops.empty()) return {};
    if (stops.size() == 1) return stops.front().color;
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (t <= stops[i].position) {
            const auto& a = stops[i - 1];
            const auto& b = stops[i];
            return Color::lerp(a.color, b.color, (t - a.position) / std::max(0.0001, b.position - a.position));
        }
    }
    return stops.back().color;
}

Color paintAt(const Paint& paint, double u, double v, int x, int y) {
    return std::visit([&](const auto& value) -> Color {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Color>) return value;
        else if constexpr (std::is_same_v<T, LinearGradient>) {
            constexpr double pi = 3.14159265358979323846;
            double radians = (value.angle - 90.0) * pi / 180.0;
            double t = std::clamp(0.5 + (u - 0.5) * std::cos(radians) + (v - 0.5) * std::sin(radians), 0.0, 1.0);
            return sampleStops(value.stops, t);
        } else if constexpr (std::is_same_v<T, RadialGradient>) {
            double t = std::min(1.0, std::hypot(u - 0.5, v - 0.5) * 1.4142);
            return sampleStops(value.stops, t);
        } else {
            int spacing = std::max(1, static_cast<int>(value.spacing)), size = std::max(1, static_cast<int>(value.size)); bool on = false;
            switch (value.kind) {
                case Pattern::Kind::Dots: { int dx = ((x % spacing) + spacing) % spacing, dy = ((y % spacing) + spacing) % spacing; on = dx*dx + dy*dy <= size*size; break; }
                case Pattern::Kind::Stripes: on = ((x + y) % spacing + spacing) % spacing < size; break;
                case Pattern::Kind::Checker: on = ((x / spacing) + (y / spacing)) % 2 == 0; break;
                case Pattern::Kind::Grid: on = ((x % spacing + spacing) % spacing < size) || ((y % spacing + spacing) % spacing < size); break;
            } return on ? value.color : Color{0,0,0,0};
        }
    }, paint);
}

void clipRoundedSpan(const Rect& r, double radius, int y, int& x0, int& x1) {
    if (radius <= 0.5) return;
    radius = std::min({radius, r.width / 2.0, r.height / 2.0});
    const double py = y + 0.5;
    double distance = 0.0;
    if (py < r.y + radius) distance = r.y + radius - py;
    else if (py > r.y + r.height - radius) distance = py - (r.y + r.height - radius);
    if (distance <= 0.0) return;

    const double chord = std::sqrt(std::max(0.0, radius * radius - distance * distance));
    const double left = r.x + radius - chord;
    const double right = r.x + r.width - radius + chord;
    x0 = std::max(x0, static_cast<int>(std::ceil(left - 0.5)));
    x1 = std::min(x1, static_cast<int>(std::ceil(right - 0.5)));
}

double roundedBoxDistance(double x, double y, const Rect& r, double radius) {
    radius = std::clamp(radius, 0.0, std::min(r.width, r.height) / 2.0);
    const double halfWidth = r.width / 2.0;
    const double halfHeight = r.height / 2.0;
    const double qx = std::abs(x - (r.x + halfWidth)) - (halfWidth - radius);
    const double qy = std::abs(y - (r.y + halfHeight)) - (halfHeight - radius);
    return std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) +
           std::min(std::max(qx, qy), 0.0) - radius;
}

double triangleExtent(double position, double start, double tooth, double depth) {
    double phase = std::fmod(position - start, tooth) / tooth;
    if (phase < 0) phase += 1.0;
    return depth * (1.0 - std::abs(phase * 2.0 - 1.0));
}

bool shapeContains(double x, double y, const Rect& rect, double radius,
                   const Sawtooth* sawtooth) {
    const bool inRect = x >= rect.x && y >= rect.y &&
                        x <= rect.x + rect.width && y <= rect.y + rect.height;
    if (inRect) {
        radius = std::clamp(radius, 0.0, std::min(rect.width, rect.height) / 2.0);
        const bool centerX = x >= rect.x + radius && x <= rect.x + rect.width - radius;
        const bool centerY = y >= rect.y + radius && y <= rect.y + rect.height - radius;
        if (radius <= 0.5 || centerX || centerY) return true;
        if (roundedBoxDistance(x, y, rect, radius) <= 0.0) return true;
    }
    if (!sawtooth) return false;
    const bool horizontal = sawtooth->side == "top" || sawtooth->side == "bottom";
    if (!horizontal && sawtooth->side != "left" && sawtooth->side != "right") return false;
    radius = std::clamp(radius, 0.0, (horizontal ? rect.width : rect.height) / 2.0);
    double alongStart = (horizontal ? rect.x : rect.y) + radius;
    double alongEnd = (horizontal ? rect.x + rect.width : rect.y + rect.height) - radius;
    const double tooth = std::max(2.0, sawtooth->tooth);
    if (alongEnd - alongStart < tooth) {
        alongStart = horizontal ? rect.x : rect.y;
        alongEnd = horizontal ? rect.x + rect.width : rect.y + rect.height;
    }
    const double along = horizontal ? x : y;
    if (along < alongStart || along > alongEnd) return false;
    const double extent = triangleExtent(along, alongStart, tooth,
                                         std::max(1.0, sawtooth->depth));
    if (sawtooth->side == "bottom") return y >= rect.y + rect.height && y <= rect.y + rect.height + extent;
    if (sawtooth->side == "top") return y <= rect.y && y >= rect.y - extent;
    if (sawtooth->side == "right") return x >= rect.x + rect.width && x <= rect.x + rect.width + extent;
    return x <= rect.x && x >= rect.x - extent;
}

Rect shapeBounds(const Rect& rect, const Sawtooth* sawtooth) {
    Rect bounds = rect;
    if (!sawtooth) return bounds;
    const double depth = std::max(1.0, sawtooth->depth);
    if (sawtooth->side == "top") { bounds.y -= depth; bounds.height += depth; }
    else if (sawtooth->side == "bottom") bounds.height += depth;
    else if (sawtooth->side == "left") { bounds.x -= depth; bounds.width += depth; }
    else if (sawtooth->side == "right") bounds.width += depth;
    return bounds;
}

double distanceToShape(double x, double y, const Rect& rect, double radius,
                       const Sawtooth* sawtooth) {
    if (shapeContains(x, y, rect, radius, sawtooth)) return 0.0;
    double distance = std::max(0.0, roundedBoxDistance(x, y, rect, radius));
    if (!sawtooth) return distance;
    const bool horizontal = sawtooth->side == "top" || sawtooth->side == "bottom";
    if (!horizontal && sawtooth->side != "left" && sawtooth->side != "right") return distance;
    radius = std::clamp(radius, 0.0, (horizontal ? rect.width : rect.height) / 2.0);
    double alongStart = (horizontal ? rect.x : rect.y) + radius;
    double alongEnd = (horizontal ? rect.x + rect.width : rect.y + rect.height) - radius;
    const double tooth = std::max(2.0, sawtooth->tooth);
    const double depth = std::max(1.0, sawtooth->depth);
    if (alongEnd - alongStart < tooth) {
        alongStart = horizontal ? rect.x : rect.y;
        alongEnd = horizontal ? rect.x + rect.width : rect.y + rect.height;
    }
    const double along = horizontal ? x : y;
    double outward = 0.0;
    if (sawtooth->side == "bottom") outward = y - (rect.y + rect.height);
    else if (sawtooth->side == "top") outward = rect.y - y;
    else if (sawtooth->side == "right") outward = x - (rect.x + rect.width);
    else outward = rect.x - x;
    if (along >= alongStart && along <= alongEnd) {
        const double extent = triangleExtent(along, alongStart, tooth, depth);
        const double slopeLength = std::sqrt(1.0 + std::pow(2.0 * depth / tooth, 2.0));
        const double toothDistance = outward > extent
            ? (outward - extent) / slopeLength
            : std::max(0.0, -outward);
        distance = std::min(distance, toothDistance);
    } else {
        const double nearestAlong = std::clamp(along, alongStart, alongEnd);
        distance = std::min(distance, std::hypot(along - nearestAlong,
                                                 std::max(0.0, outward)));
    }
    return distance;
}

Vec3 rotateEuler(Vec3 value, Vec3 degrees) {
    constexpr double radians = 3.14159265358979323846 / 180.0;
    const double cx = std::cos(degrees.x * radians), sx = std::sin(degrees.x * radians);
    const double cy = std::cos(degrees.y * radians), sy = std::sin(degrees.y * radians);
    const double cz = std::cos(degrees.z * radians), sz = std::sin(degrees.z * radians);
    value = {value.x, value.y * cx - value.z * sx, value.y * sx + value.z * cx};
    value = {value.x * cy + value.z * sy, value.y, -value.x * sy + value.z * cy};
    return {value.x * cz - value.y * sz, value.x * sz + value.y * cz, value.z};
}

Vec3 inverseRotateEuler(Vec3 value, Vec3 degrees) {
    constexpr double radians = 3.14159265358979323846 / 180.0;
    const double cz = std::cos(-degrees.z * radians), sz = std::sin(-degrees.z * radians);
    const double cy = std::cos(-degrees.y * radians), sy = std::sin(-degrees.y * radians);
    const double cx = std::cos(-degrees.x * radians), sx = std::sin(-degrees.x * radians);
    value = {value.x * cz - value.y * sz, value.x * sz + value.y * cz, value.z};
    value = {value.x * cy + value.z * sy, value.y, -value.x * sy + value.z * cy};
    return {value.x, value.y * cx - value.z * sx, value.y * sx + value.z * cx};
}

Color litColor(Color color, double light) {
    light = std::clamp(light, 0.18, 1.0);
    color.r = static_cast<std::uint8_t>(std::lround(color.r * light));
    color.g = static_cast<std::uint8_t>(std::lround(color.g * light));
    color.b = static_cast<std::uint8_t>(std::lround(color.b * light));
    return color;
}

Color modulateColor(Color color, Color tint) {
    color.r=static_cast<std::uint8_t>(color.r*tint.r/255);
    color.g=static_cast<std::uint8_t>(color.g*tint.g/255);
    color.b=static_cast<std::uint8_t>(color.b*tint.b/255);
    color.a=static_cast<std::uint8_t>(color.a*tint.a/255);
    return color;
}

Paint modulatePaint(Paint paint, Color tint) {
    std::visit([&](auto& value) {
        using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,Color>) value=modulateColor(value,tint);
        else if constexpr(std::is_same_v<T,LinearGradient>||std::is_same_v<T,RadialGradient>)
            for(ColorStop& stop:value.stops)stop.color=modulateColor(stop.color,tint);
        else value.color=modulateColor(value.color,tint);
    },paint);
    return paint;
}
}

SoftwareRenderer::SoftwareRenderer(int logicalWidth, int logicalHeight,
                                   std::filesystem::path output, double renderScale)
    : width_(std::max(1, static_cast<int>(std::lround(logicalWidth * renderScale)))),
      height_(std::max(1, static_cast<int>(std::lround(logicalHeight * renderScale)))),
      logicalWidth_(logicalWidth), logicalHeight_(logicalHeight),
      renderScale_(static_cast<double>(width_) / std::max(1, logicalWidth)),
      output_(std::move(output)),
      tvPixels_(static_cast<std::size_t>(width_ * height_)),
      gamePadPixels_(static_cast<std::size_t>(width_ * height_)),
      currentPixels_(&tvPixels_),
      depthBuffer_(static_cast<std::size_t>(width_ * height_)) {}

void SoftwareRenderer::beginDisplay(Display display, Color clear) {
    currentDisplay_ = display;
    currentPixels_ = display == Display::GamePad ? &gamePadPixels_ : &tvPixels_;
    std::fill(currentPixels_->begin(), currentPixels_->end(), clear);
    std::fill(depthBuffer_.begin(), depthBuffer_.end(), 1.0e30);
}

SoftwareRenderer::~SoftwareRenderer() = default;

Node SoftwareRenderer::scaledNode(const Node& source) const {
    Node node;
    node.type = source.type;
    node.transform = source.worldTransform();
    node.transform.position = node.transform.position * renderScale_;
    node.transform3D = source.transform3D;
    node.width = source.width * renderScale_;
    node.height = source.height * renderScale_;
    node.depth = source.depth * renderScale_;
    node.opacity = source.opacity;
    node.text = source.text;
    node.source = source.source;
    node.tint = source.tint;
    node.hue = source.hue;
    node.saturation = source.saturation;
    node.attributes = source.attributes;
    return node;
}

ComputedStyle SoftwareRenderer::scaledStyle(const ComputedStyle& source) const {
    ComputedStyle style = source;
    style.borderWidth *= renderScale_;
    style.borderRadius *= renderScale_;
    style.fontSize *= renderScale_;
    for (Shadow& shadow : style.shadows) {
        shadow.x *= renderScale_;
        shadow.y *= renderScale_;
        shadow.blur *= renderScale_;
    }
    if (style.sawtooth) {
        style.sawtooth->tooth *= renderScale_;
        style.sawtooth->depth *= renderScale_;
    }
    auto scalePaint = [this](Paint& paint) {
        if (auto* pattern = std::get_if<Pattern>(&paint)) {
            pattern->size *= renderScale_;
            pattern->spacing *= renderScale_;
        }
    };
    if (style.fill) scalePaint(*style.fill);
    for (Paint& paint : style.backgrounds) scalePaint(paint);
    return style;
}

void SoftwareRenderer::blendPixel(Color& dest, Color source, double opacity) {
    const int srcA = static_cast<int>(source.a * std::clamp(opacity, 0.0, 1.0) + 0.5);
    if (srcA <= 0) return;
    if (currentBlendMode_ == BlendMode::Additive) {
        dest.r = static_cast<std::uint8_t>(std::min(255, dest.r + source.r * srcA / 255));
        dest.g = static_cast<std::uint8_t>(std::min(255, dest.g + source.g * srcA / 255));
        dest.b = static_cast<std::uint8_t>(std::min(255, dest.b + source.b * srcA / 255));
        dest.a = 255; return;
    }
    if (currentBlendMode_ == BlendMode::Subtract) {
        dest.r = static_cast<std::uint8_t>(std::max(0, dest.r - source.r * srcA / 255));
        dest.g = static_cast<std::uint8_t>(std::max(0, dest.g - source.g * srcA / 255));
        dest.b = static_cast<std::uint8_t>(std::max(0, dest.b - source.b * srcA / 255));
        dest.a = 255; return;
    }
    if (currentBlendMode_ == BlendMode::Multiply || currentBlendMode_ == BlendMode::Screen) {
        auto mixChannel = [&](int destination, int incoming) {
            const int blended = currentBlendMode_ == BlendMode::Multiply
                ? destination * incoming / 255
                : 255 - (255 - destination) * (255 - incoming) / 255;
            return static_cast<std::uint8_t>((blended * srcA + destination * (255-srcA)) / 255);
        };
        dest.r=mixChannel(dest.r,source.r); dest.g=mixChannel(dest.g,source.g);
        dest.b=mixChannel(dest.b,source.b); dest.a=255; return;
    }
    if (srcA >= 255) {
        dest = source;
        dest.a = 255;
        return;
    }
    const int inv = 255 - srcA;
    const int destinationAlpha = dest.a;
    const int outputAlpha = srcA + destinationAlpha * inv / 255;
    if (outputAlpha <= 0) { dest = {}; return; }
    dest.r = static_cast<std::uint8_t>((source.r * srcA +
        dest.r * destinationAlpha * inv / 255) / outputAlpha);
    dest.g = static_cast<std::uint8_t>((source.g * srcA +
        dest.g * destinationAlpha * inv / 255) / outputAlpha);
    dest.b = static_cast<std::uint8_t>((source.b * srcA +
        dest.b * destinationAlpha * inv / 255) / outputAlpha);
    dest.a = static_cast<std::uint8_t>(outputAlpha);
}

void SoftwareRenderer::pixel(int x, int y, Color source, double opacity) {
    if (!currentPixels_ || x < 0 || y < 0 || x >= width_ || y >= height_) return;
    Color& dest = (*currentPixels_)[static_cast<std::size_t>(y * width_ + x)];
    blendPixel(dest, source, opacity);
}

void SoftwareRenderer::fillSpan(int y, int x0, int x1, Color color, double opacity) {
    if (!currentPixels_ || y < 0 || y >= height_) return;
    x0 = std::max(0, x0);
    x1 = std::min(width_, x1);
    if (x0 >= x1) return;
    const int srcA = static_cast<int>(color.a * std::clamp(opacity, 0.0, 1.0) + 0.5);
    Color* row = currentPixels_->data() + static_cast<std::size_t>(y * width_);
    if (currentBlendMode_ != BlendMode::Normal) {
        for (int x=x0;x<x1;++x) blendPixel(row[x],color,opacity);
        return;
    }
    if (srcA >= 255) {
        color.a = 255;
        std::fill(row + x0, row + x1, color);
        return;
    }
    if (srcA <= 0) return;
    const int inv = 255 - srcA;
    for (int x = x0; x < x1; ++x) {
        Color& dest = row[x];
        if (dest.a < 255) {
            // Ordered Wii U software layers start transparent. Preserve their
            // coverage so the GX2 pass can composite them over earlier quads.
            blendPixel(dest, color, opacity);
            continue;
        }
        dest.r = static_cast<std::uint8_t>((color.r * srcA + dest.r * inv) / 255);
        dest.g = static_cast<std::uint8_t>((color.g * srcA + dest.g * inv) / 255);
        dest.b = static_cast<std::uint8_t>((color.b * srcA + dest.b * inv) / 255);
        dest.a = 255;
    }
}

void SoftwareRenderer::paintGrid(const Rect& r, const Pattern& pattern, double opacity, double radius) {
    const int spacing = std::max(1, static_cast<int>(pattern.spacing));
    const int size = std::max(1, static_cast<int>(pattern.size));
    const int x0 = std::max(0, static_cast<int>(std::floor(r.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(r.y)));
    const int x1 = std::min(width_, static_cast<int>(std::ceil(r.x + r.width)));
    const int y1 = std::min(height_, static_cast<int>(std::ceil(r.y + r.height)));
    auto first = [spacing](int start) {
        int aligned = start - ((start % spacing) + spacing) % spacing;
        return aligned;
    };
    for (int gx = first(x0); gx < x1; gx += spacing) {
        const int xs = std::max(x0, gx);
        const int xe = std::min(x1, gx + size);
        for (int y = y0; y < y1; ++y) {
            int clippedStart = x0;
            int clippedEnd = x1;
            clipRoundedSpan(r, radius, y, clippedStart, clippedEnd);
            fillSpan(y, std::max(xs, clippedStart), std::min(xe, clippedEnd), pattern.color, opacity);
        }
    }
    for (int gy = first(y0); gy < y1; gy += spacing) {
        const int ys = std::max(y0, gy);
        const int ye = std::min(y1, gy + size);
        for (int y = ys; y < ye; ++y) {
            int clippedStart = x0;
            int clippedEnd = x1;
            clipRoundedSpan(r, radius, y, clippedStart, clippedEnd);
            fillSpan(y, clippedStart, clippedEnd, pattern.color, opacity);
        }
    }
}

void SoftwareRenderer::paintDots(const Rect& r, const Pattern& pattern, double opacity, double radius) {
    const int spacing = std::max(1, static_cast<int>(pattern.spacing));
    const int size = std::max(1, static_cast<int>(pattern.size));
    const int r2 = size * size;
    const int x0 = std::max(0, static_cast<int>(std::floor(r.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(r.y)));
    const int x1 = std::min(width_, static_cast<int>(std::ceil(r.x + r.width)));
    const int y1 = std::min(height_, static_cast<int>(std::ceil(r.y + r.height)));
    int cy0 = y0 - ((y0 % spacing) + spacing) % spacing;
    int cx0 = x0 - ((x0 % spacing) + spacing) % spacing;
    for (int cy = cy0; cy <= y1; cy += spacing) {
        for (int cx = cx0; cx <= x1; cx += spacing) {
            for (int dy = -size; dy <= size; ++dy) {
                const int y = cy + dy;
                if (y < y0 || y >= y1) continue;
                int clippedStart = x0;
                int clippedEnd = x1;
                clipRoundedSpan(r, radius, y, clippedStart, clippedEnd);
                for (int dx = -size; dx <= size; ++dx) {
                    if (dx * dx + dy * dy > r2) continue;
                    const int x = cx + dx;
                    if (x < clippedStart || x >= clippedEnd) continue;
                    pixel(x, y, pattern.color, opacity);
                }
            }
        }
    }
}

void SoftwareRenderer::paintRect(const Rect& r, const Paint& paint, double opacity, double radius) {
    int x0 = std::max(0, static_cast<int>(std::floor(r.x)));
    int y0 = std::max(0, static_cast<int>(std::floor(r.y)));
    int x1 = std::min(width_, static_cast<int>(std::ceil(r.x + r.width)));
    int y1 = std::min(height_, static_cast<int>(std::ceil(r.y + r.height)));
    if (x0 >= x1 || y0 >= y1) return;

    if (const auto* color = std::get_if<Color>(&paint)) {
        for (int y = y0; y < y1; ++y) {
            int spanStart = x0;
            int spanEnd = x1;
            clipRoundedSpan(r, radius, y, spanStart, spanEnd);
            fillSpan(y, spanStart, spanEnd, *color, opacity);
        }
        return;
    }
    if (const auto* gradient = std::get_if<LinearGradient>(&paint)) {
        double angle = std::fmod(gradient->angle, 360.0);
        if (angle < 0) angle += 360.0;
        const bool vertical = std::abs(angle - 180.0) < 0.5 || std::abs(angle) < 0.5 || std::abs(angle - 360.0) < 0.5;
        const bool horizontal = std::abs(angle - 90.0) < 0.5 || std::abs(angle - 270.0) < 0.5;
        if (vertical) {
            const bool invert = std::abs(angle) < 0.5 || std::abs(angle - 360.0) < 0.5;
            const double height = std::max(1.0, r.height);
            for (int y = y0; y < y1; ++y) {
                double v = (y + 0.5 - r.y) / height;
                if (invert) v = 1.0 - v;
                int spanStart = x0;
                int spanEnd = x1;
                clipRoundedSpan(r, radius, y, spanStart, spanEnd);
                fillSpan(y, spanStart, spanEnd, sampleStops(gradient->stops, v), opacity);
            }
            return;
        }
        if (horizontal) {
            const bool invert = std::abs(angle - 270.0) < 0.5;
            const double width = std::max(1.0, r.width);
            std::vector<Color> colors(static_cast<std::size_t>(x1 - x0));
            for (int x = x0; x < x1; ++x) {
                double u = (x + 0.5 - r.x) / width;
                if (invert) u = 1.0 - u;
                colors[static_cast<std::size_t>(x - x0)] = sampleStops(gradient->stops, u);
            }
            for (int y = y0; y < y1; ++y) {
                int spanStart = x0;
                int spanEnd = x1;
                clipRoundedSpan(r, radius, y, spanStart, spanEnd);
                for (int x = spanStart; x < spanEnd; ++x)
                    pixel(x, y, colors[static_cast<std::size_t>(x - x0)], opacity);
            }
            return;
        }
    }
    if (const auto* pattern = std::get_if<Pattern>(&paint)) {
        if (pattern->kind == Pattern::Kind::Grid) {
            paintGrid(r, *pattern, opacity, radius);
            return;
        }
        if (pattern->kind == Pattern::Kind::Dots) {
            paintDots(r, *pattern, opacity, radius);
            return;
        }
    }

    for (int y = y0; y < y1; ++y) {
        int spanStart = x0;
        int spanEnd = x1;
        clipRoundedSpan(r, radius, y, spanStart, spanEnd);
        for (int x = spanStart; x < spanEnd; ++x) {
            pixel(x, y, paintAt(paint, (x - r.x) / std::max(1.0, r.width), (y - r.y) / std::max(1.0, r.height), x, y),
                  opacity);
        }
    }
}

void SoftwareRenderer::shadow(const Rect& r, const Shadow& s, double radius,
                              const Sawtooth* sawtooth) {
    if (s.inset) {
        const double blur = std::max(1.0, s.blur);
        const int x0 = std::max(0, static_cast<int>(std::floor(r.x)));
        const int y0 = std::max(0, static_cast<int>(std::floor(r.y)));
        const int x1 = std::min(width_, static_cast<int>(std::ceil(r.x + r.width)));
        const int y1 = std::min(height_, static_cast<int>(std::ceil(r.y + r.height)));
        for (int y = y0; y < y1; ++y) {
            int spanStart = x0;
            int spanEnd = x1;
            clipRoundedSpan(r, radius, y, spanStart, spanEnd);
            for (int x = spanStart; x < spanEnd; ++x) {
                const double px = x + 0.5;
                const double py = y + 0.5;
                const double sampleX = px - s.x;
                const double sampleY = py - s.y;
                const double safeInset = radius + blur;
                if (sampleX >= r.x + safeInset && sampleX <= r.x + r.width - safeInset &&
                    sampleY >= r.y + safeInset && sampleY <= r.y + r.height - safeInset)
                    continue;
                const double distance = -roundedBoxDistance(sampleX, sampleY, r, radius);
                if (distance >= blur) continue;
                pixel(x, y, s.color, std::clamp(1.0 - distance / blur, 0.0, 1.0));
            }
        }
        return;
    }
    const Rect shifted{r.x + s.x, r.y + s.y, r.width, r.height};
    if (s.blur <= 0.5) {
        paintRect(shifted, s.color, 1.0, radius);
        if (sawtooth) paintSawtooth(shifted, *sawtooth, s.color, 1.0, radius);
        return;
    }
    // Treat the CSS blur value as roughly two standard deviations. An erfc
    // falloff gives a soft convolution-like edge instead of the old uniformly
    // expanded rectangle, and distanceToShape follows teeth as well as curves.
    const double sigma = std::max(0.5, s.blur * 0.5);
    const double margin = std::ceil(sigma * 3.0) + 1.0;
    Rect bounds = shapeBounds(shifted, sawtooth);
    const int x0 = std::max(0, static_cast<int>(std::floor(bounds.x - margin)));
    const int y0 = std::max(0, static_cast<int>(std::floor(bounds.y - margin)));
    const int x1 = std::min(width_, static_cast<int>(std::ceil(bounds.x + bounds.width + margin)));
    const int y1 = std::min(height_, static_cast<int>(std::ceil(bounds.y + bounds.height + margin)));
    constexpr double inverseRootTwo = 0.7071067811865475244;
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
        const double px = x + 0.5;
        const double py = y + 0.5;
        const bool inside = shapeContains(px, py, shifted, radius, sawtooth);
        const double distance = inside ? 0.0 : distanceToShape(px, py, shifted, radius, sawtooth);
        const double coverage = inside ? 0.5 :
            0.5 * std::erfc(distance * inverseRootTwo / sigma);
        if (coverage > 0.002) pixel(x, y, s.color, coverage);
    }
}

void SoftwareRenderer::paintSawtooth(const Rect& r, const Sawtooth& s, const Paint& paint,
                                     double opacity, double radius, double inset) {
    const bool horizontal = s.side == "top" || s.side == "bottom";
    if (!horizontal && s.side != "left" && s.side != "right") return;

    const double tooth = std::max(2.0, s.tooth);
    const double depth = std::max(1.0, s.depth);
    radius = std::clamp(radius, 0.0, (horizontal ? r.width : r.height) / 2.0);
    double alongStart = (horizontal ? r.x : r.y) + radius;
    double alongEnd = (horizontal ? r.x + r.width : r.y + r.height) - radius;
    if (alongEnd - alongStart < tooth) {
        alongStart = horizontal ? r.x : r.y;
        alongEnd = horizontal ? r.x + r.width : r.y + r.height;
    }

    const double slope = 2.0 * depth / tooth;
    const double edgeInset = std::max(0.0, inset) * std::sqrt(1.0 + slope * slope);
    const int first = static_cast<int>(std::floor(alongStart));
    const int last = static_cast<int>(std::ceil(alongEnd));
    const int depthPixels = static_cast<int>(std::ceil(depth));
    for (int along = first; along < last; ++along) {
        const double center = along + 0.5;
        if (center < alongStart || center >= alongEnd) continue;
        const double extent = triangleExtent(center, alongStart, tooth, depth) - edgeInset;
        if (extent <= 0) continue;
        for (int d = 0; d < depthPixels && d + 0.5 < extent; ++d) {
            int x = 0;
            int y = 0;
            if (s.side == "bottom") {
                x = along;
                y = static_cast<int>(std::floor(r.y + r.height)) + d;
            } else if (s.side == "top") {
                x = along;
                y = static_cast<int>(std::ceil(r.y)) - d - 1;
            } else if (s.side == "right") {
                x = static_cast<int>(std::floor(r.x + r.width)) + d;
                y = along;
            } else {
                x = static_cast<int>(std::ceil(r.x)) - d - 1;
                y = along;
            }
            const double u = (x + 0.5 - r.x) / std::max(1.0, r.width);
            const double v = (y + 0.5 - r.y) / std::max(1.0, r.height);
            pixel(x, y, paintAt(paint, u, v, x, y), opacity);
        }
    }
}

void SoftwareRenderer::shadeSawtoothInset(const Rect& r, const Sawtooth& s,
                                          const Shadow& shadow, double radius,
                                          double borderWidth) {
    const bool horizontal = s.side == "top" || s.side == "bottom";
    if (!horizontal && s.side != "left" && s.side != "right") return;

    const double tooth = std::max(2.0, s.tooth);
    const double depth = std::max(1.0, s.depth);
    const double slope = 2.0 * depth / tooth;
    const double slopeLength = std::sqrt(1.0 + slope * slope);
    const double borderInset = std::max(0.0, borderWidth) * slopeLength;
    const double blur = std::max(1.0, shadow.blur);
    radius = std::clamp(radius, 0.0, (horizontal ? r.width : r.height) / 2.0);
    double alongStart = (horizontal ? r.x : r.y) + radius;
    double alongEnd = (horizontal ? r.x + r.width : r.y + r.height) - radius;
    if (alongEnd - alongStart < tooth) {
        alongStart = horizontal ? r.x : r.y;
        alongEnd = horizontal ? r.x + r.width : r.y + r.height;
    }

    const int first = static_cast<int>(std::floor(alongStart));
    const int last = static_cast<int>(std::ceil(alongEnd));
    const int depthPixels = static_cast<int>(std::ceil(depth));
    for (int along = first; along < last; ++along) {
        const double center = along + 0.5;
        if (center < alongStart || center >= alongEnd) continue;
        const double actualExtent = triangleExtent(center, alongStart, tooth, depth) - borderInset;
        if (actualExtent <= 0) continue;
        for (int d = 0; d < depthPixels && d + 0.5 < actualExtent; ++d) {
            int x = 0;
            int y = 0;
            double sampleAlong = center;
            double sampleOutward = d + 0.5;
            if (s.side == "bottom") {
                x = along; y = static_cast<int>(std::floor(r.y + r.height)) + d;
                sampleAlong -= shadow.x; sampleOutward -= shadow.y;
            } else if (s.side == "top") {
                x = along; y = static_cast<int>(std::ceil(r.y)) - d - 1;
                sampleAlong -= shadow.x; sampleOutward += shadow.y;
            } else if (s.side == "right") {
                x = static_cast<int>(std::floor(r.x + r.width)) + d; y = along;
                sampleAlong -= shadow.y; sampleOutward -= shadow.x;
            } else {
                x = static_cast<int>(std::ceil(r.x)) - d - 1; y = along;
                sampleAlong -= shadow.y; sampleOutward += shadow.x;
            }
            const double sampleExtent = triangleExtent(sampleAlong, alongStart, tooth, depth) - borderInset;
            const double distance = (sampleExtent - sampleOutward) / slopeLength;
            if (distance >= blur) continue;
            pixel(x, y, shadow.color, std::clamp(1.0 - distance / blur, 0.0, 1.0));
        }
    }
}

void SoftwareRenderer::paintStyledShape(const Rect& rect, const ComputedStyle& style,
                                        double opacity) {
    const Sawtooth* sawtooth = style.sawtooth ? &*style.sawtooth : nullptr;
    // Rounded rectangles have an exact, span-based fast path. Besides being
    // much cheaper than per-pixel erosion, painting an outer rounded box and
    // then its inset gives an exact constant-width curved outline.
    if (!sawtooth) {
        Rect content = rect;
        double contentRadius = style.borderRadius;
        if (style.borderWidth > 0) {
            paintRect(rect, style.borderColor, opacity, style.borderRadius);
            content = {rect.x + style.borderWidth, rect.y + style.borderWidth,
                       rect.width - style.borderWidth * 2,
                       rect.height - style.borderWidth * 2};
            contentRadius = std::max(0.0, style.borderRadius - style.borderWidth);
        }
        if (content.width <= 0 || content.height <= 0) return;
        if (style.fill) paintRect(content, *style.fill, opacity, contentRadius);
        for (const Paint& background : style.backgrounds)
            paintRect(content, background, opacity, contentRadius);
        return;
    }

    // Paint the body and teeth as one uninterrupted fill first. The outline
    // is applied afterward only around the exposed silhouette, so there is no
    // false border at the rectangle/tooth join.
    if (style.fill) {
        paintRect(rect, *style.fill, opacity, style.borderRadius);
        paintSawtooth(rect, *sawtooth, *style.fill, opacity, style.borderRadius);
    }
    for (const Paint& background : style.backgrounds) {
        paintRect(rect, background, opacity, style.borderRadius);
        paintSawtooth(rect, *sawtooth, background, opacity, style.borderRadius);
    }
    const double stroke = std::max(0.0, style.borderWidth);
    if (stroke <= 0.0) return;

    const Rect bounds = shapeBounds(rect, sawtooth);
    const int x0 = std::max(0, static_cast<int>(std::floor(bounds.x)) - 1);
    const int y0 = std::max(0, static_cast<int>(std::floor(bounds.y)) - 1);
    const int x1 = std::min(width_, static_cast<int>(std::ceil(bounds.x + bounds.width)) + 1);
    const int y1 = std::min(height_, static_cast<int>(std::ceil(bounds.y + bounds.height)) + 1);
    constexpr std::array<Vec2, 4> samples{{
        {0.25, 0.25}, {0.75, 0.25}, {0.25, 0.75}, {0.75, 0.75}
    }};
    constexpr std::array<Vec2, 16> ring{{
        {1,0}, {0.9238795,0.3826834}, {0.7071068,0.7071068}, {0.3826834,0.9238795},
        {0,1}, {-0.3826834,0.9238795}, {-0.7071068,0.7071068}, {-0.9238795,0.3826834},
        {-1,0}, {-0.9238795,-0.3826834}, {-0.7071068,-0.7071068}, {-0.3826834,-0.9238795},
        {0,-1}, {0.3826834,-0.9238795}, {0.7071068,-0.7071068}, {0.9238795,-0.3826834}
    }};
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
        const double centerX = x + 0.5;
        const double centerY = y + 0.5;
        const double edgeBand = stroke + 1.0;
        if (centerX >= rect.x + edgeBand &&
            centerX <= rect.x + rect.width - edgeBand &&
            centerY >= rect.y + edgeBand &&
            centerY <= rect.y + rect.height - edgeBand) continue;
        int borderSamples = 0;
        for (const Vec2 sample : samples) {
            const double px = x + sample.x;
            const double py = y + sample.y;
            if (!shapeContains(px, py, rect, style.borderRadius, sawtooth)) continue;
            bool interior = true;
            for (const Vec2 direction : ring) {
                if (!shapeContains(px + direction.x * stroke,
                                   py + direction.y * stroke,
                                   rect, style.borderRadius, sawtooth)) {
                    interior = false;
                    break;
                }
            }
            if (!interior) ++borderSamples;
        }
        if (borderSamples) pixel(x, y, style.borderColor,
                                 opacity * borderSamples / samples.size());
    }
}

void SoftwareRenderer::drawRectanglePixels(const Node& node, const ComputedStyle& style) {
    Rect r = node.bounds();
    const Sawtooth* teeth = style.sawtooth ? &*style.sawtooth : nullptr;
    for (auto& s : style.shadows) if (!s.inset) shadow(r, s, style.borderRadius, teeth);

    paintStyledShape(r, style, node.opacity);
    Rect content = {r.x + style.borderWidth, r.y + style.borderWidth,
                    r.width - style.borderWidth * 2, r.height - style.borderWidth * 2};
    const double contentRadius = std::max(0.0, style.borderRadius - style.borderWidth);
    for (auto& s : style.shadows) if (s.inset) {
        shadow(content, s, contentRadius);
        if (teeth) shadeSawtoothInset(r, *teeth, s, style.borderRadius,
                                      style.borderWidth);
    }
}

void SoftwareRenderer::drawRectangle(const Node& node, const ComputedStyle& style) {
    const BlendMode previous = currentBlendMode_;
    currentBlendMode_ = style.blendMode;
    drawRectanglePixels(scaledNode(node), scaledStyle(style));
    currentBlendMode_ = previous;
}

void SoftwareRenderer::drawPanel(const Node& node, const ComputedStyle& style) {
    drawRectangle(node, style);
}

void SoftwareRenderer::drawSprite(const Node& node, const ComputedStyle& style, const Asset* texture) {
    const BlendMode previousBlend = currentBlendMode_;
    currentBlendMode_ = style.blendMode;
    Node physicalNode = scaledNode(node);
    ComputedStyle actual = scaledStyle(style);
    const Node& drawNode = physicalNode;
    const bool hasTexture = texture && !texture->pixels.empty();
    if (!actual.fill && !hasTexture) actual.fill = Color{255, 190, 70, 255};
    bool visibleFill = actual.fill.has_value();
    if (visibleFill) {
        if (const auto* solid = std::get_if<Color>(&*actual.fill))
            visibleFill = solid->a != 0;
    }
    const bool decorated = visibleFill || !actual.backgrounds.empty() ||
        actual.borderWidth > 0 || !actual.shadows.empty() || actual.sawtooth.has_value();
    if (std::abs(drawNode.transform.rotation) < 0.0001 && decorated)
        drawRectanglePixels(drawNode, actual);
    else if (!hasTexture) {
        constexpr double pi = 3.14159265358979323846;
        Rect r = drawNode.bounds(); double cx=r.x+r.width/2, cy=r.y+r.height/2, angle=-drawNode.transform.rotation*pi/180.0;
        const double cosA = std::cos(angle), sinA = std::sin(angle);
        double extentX=std::abs(cosA)*r.width/2+std::abs(sinA)*r.height/2;
        double extentY=std::abs(sinA)*r.width/2+std::abs(cosA)*r.height/2;
        const int y0=std::max(0,static_cast<int>(std::floor(cy-extentY)));
        const int y1=std::min(height_,static_cast<int>(std::ceil(cy+extentY)));
        const int x0=std::max(0,static_cast<int>(std::floor(cx-extentX)));
        const int x1=std::min(width_,static_cast<int>(std::ceil(cx+extentX)));
        for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x){
            double dx=x+.5-cx,dy=y+.5-cy,lx=dx*cosA-dy*sinA+r.width/2,ly=dx*sinA+dy*cosA+r.height/2;
            if(lx>=0&&ly>=0&&lx<r.width&&ly<r.height){pixel(x,y,paintAt(*actual.fill,lx/r.width,ly/r.height,x,y),drawNode.opacity);for(auto&bg:actual.backgrounds)pixel(x,y,paintAt(bg,lx/r.width,ly/r.height,x,y),drawNode.opacity);}
        }
    }
    if (hasTexture) {
        // A one-pixel-wide gradient stretched across the display has one
        // color per scanline. Sampling and blending it pixel by pixel wastes
        // most of the frame on the Wii U's CPU.
        const Rect background = drawNode.bounds();
        if (texture->width == 1 && texture->height == height_ &&
            !decorated && actual.borderRadius <= 0 &&
            std::abs(drawNode.transform.rotation) < 0.0001 &&
            background.x == 0 && background.y == 0 &&
            background.width == width_ && background.height == height_ &&
            drawNode.tint.r == 255 && drawNode.tint.g == 255 &&
            drawNode.tint.b == 255 && drawNode.tint.a == 255) {
            for (int y = 0; y < height_; ++y) {
                const int sourceY = std::min(texture->height - 1,
                    y * texture->height / height_);
                fillSpan(y, 0, width_, texture->pixels[static_cast<std::size_t>(sourceY)],
                         drawNode.opacity);
            }
            currentBlendMode_ = previousBlend;
            return;
        }
        constexpr double pi = 3.14159265358979323846; Rect r=drawNode.bounds(); double cx=r.x+r.width/2,cy=r.y+r.height/2,angle=-drawNode.transform.rotation*pi/180.0;
        const double cosA = std::cos(angle), sinA = std::sin(angle);
        double extentX=std::abs(cosA)*r.width/2+std::abs(sinA)*r.height/2,extentY=std::abs(sinA)*r.width/2+std::abs(cosA)*r.height/2;
        const int y0=std::max(0,static_cast<int>(std::floor(cy-extentY)));
        const int y1=std::min(height_,static_cast<int>(std::ceil(cy+extentY)));
        const int x0=std::max(0,static_cast<int>(std::floor(cx-extentX)));
        const int x1=std::min(width_,static_cast<int>(std::ceil(cx+extentX)));
        for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x){
            double dx=x+.5-cx,dy=y+.5-cy,lx=dx*cosA-dy*sinA+r.width/2,ly=dx*sinA+dy*cosA+r.height/2;
            if(lx<0||ly<0||lx>=r.width||ly>=r.height)continue;
            if (std::abs(drawNode.transform.rotation) < 0.0001) {
                Rect clipRect = r;
                double clipRadius = actual.borderRadius;
                const Sawtooth* clipTeeth = actual.sawtooth ? &*actual.sawtooth : nullptr;
                if (!clipTeeth && actual.borderWidth > 0) {
                    clipRect = {r.x + actual.borderWidth, r.y + actual.borderWidth,
                                r.width - actual.borderWidth * 2,
                                r.height - actual.borderWidth * 2};
                    clipRadius = std::max(0.0, actual.borderRadius - actual.borderWidth);
                }
                if (!shapeContains(x + 0.5, y + 0.5, clipRect, clipRadius, clipTeeth)) continue;
            }
            int tx=std::clamp(static_cast<int>(lx/std::max(1.0,r.width)*texture->width),0,texture->width-1);
            int ty=std::clamp(static_cast<int>(ly/std::max(1.0,r.height)*texture->height),0,texture->height-1);
            const Color sample=applyHueSaturation(
                texture->pixels[static_cast<std::size_t>(ty*texture->width+tx)],
                drawNode.hue,drawNode.saturation);
            pixel(x,y,modulateColor(sample,drawNode.tint),drawNode.opacity);
        }
        if (std::abs(drawNode.transform.rotation) < 0.0001 &&
            actual.borderWidth > 0 && actual.sawtooth) {
            ComputedStyle outline = actual;
            outline.fill.reset();
            outline.backgrounds.clear();
            outline.shadows.clear();
            paintStyledShape(r, outline, drawNode.opacity);
        }
    }
    // A diagonal mark makes missing/placeholder image assets obvious in the dependency-free preview.
    if (!drawNode.source.empty() && (!texture || texture->pixels.empty())) { Rect r=drawNode.bounds(); int n=static_cast<int>(std::min(r.width,r.height)); for(int i=0;i<n;++i) pixel(static_cast<int>(r.x)+i,static_cast<int>(r.y)+i,{255,255,255,150},drawNode.opacity); }
    currentBlendMode_ = previousBlend;
}

void SoftwareRenderer::drawText(const Node& node, const ComputedStyle& style,
                                const Asset* fontAsset) {
    Node physicalNode = scaledNode(node);
    ComputedStyle physicalStyle = scaledStyle(style);
    const Node& drawNode = physicalNode;
    const ComputedStyle& drawStyle = physicalStyle;
    if (drawNode.text.empty()) return;
    const BlendMode previousBlend = currentBlendMode_;
    currentBlendMode_ = drawStyle.blendMode;
    EmbeddedFont* embedded = &EmbeddedFont::instance();
    FontFace* external = nullptr;
    if (fontAsset && fontAsset->available && !fontAsset->bytes.empty()) {
        const std::string key = fontAsset->path.string();
        auto& cached = fonts_[key];
        if (!cached) cached = std::make_unique<FontFace>(fontAsset->bytes);
        if (cached->ready()) external = cached.get();
    }
    const float pixelHeight = static_cast<float>(std::max(4.0, drawStyle.fontSize));
    if (!external && !embedded->ready()) { currentBlendMode_ = previousBlend; return; }
    auto ascent = [&](float size) { return external ? external->ascent(size) : embedded->ascent(size); };
    auto kerning = [&](int previous, int current, float size) {
        return external ? external->kerning(previous, current, size) :
                          embedded->kerning(previous, current, size);
    };
    auto glyphFor = [&](int codepoint, int size) -> const GlyphBitmap& {
        return external ? external->glyph(codepoint, size) : embedded->glyph(codepoint, size);
    };
    const float lineHeight = pixelHeight * 1.2f;
    float lineTop = static_cast<float>(drawNode.transform.position.y);
    float baseline = lineTop + ascent(pixelHeight);
    float cursor = static_cast<float>(drawNode.transform.position.x);
    int previous = 0;
    std::string_view remaining = drawNode.text;
    auto nextCodepoint = [](std::string_view& text, int& codepoint) {
        if (text.empty()) return false;
        const auto b = static_cast<unsigned char>(text.front());
        if (b < 0x80) { codepoint = b; text.remove_prefix(1); return true; }
        if ((b & 0xE0) == 0xC0 && text.size() >= 2) {
            codepoint = ((b & 0x1F) << 6) | (static_cast<unsigned char>(text[1]) & 0x3F);
            text.remove_prefix(2); return true;
        }
        if ((b & 0xF0) == 0xE0 && text.size() >= 3) {
            codepoint = ((b & 0x0F) << 12) | ((static_cast<unsigned char>(text[1]) & 0x3F) << 6) |
                        (static_cast<unsigned char>(text[2]) & 0x3F);
            text.remove_prefix(3); return true;
        }
        if ((b & 0xF8) == 0xF0 && text.size() >= 4) {
            codepoint = ((b & 0x07) << 18) | ((static_cast<unsigned char>(text[1]) & 0x3F) << 12) |
                        ((static_cast<unsigned char>(text[2]) & 0x3F) << 6) | (static_cast<unsigned char>(text[3]) & 0x3F);
            text.remove_prefix(4); return true;
        }
        codepoint = b; text.remove_prefix(1); return true;
    };

    int codepoint = 0;
    while (nextCodepoint(remaining, codepoint)) {
        if (codepoint == '\n') {
            cursor = static_cast<float>(drawNode.transform.position.x);
            lineTop += lineHeight;
            baseline = lineTop + ascent(pixelHeight);
            previous = 0;
            continue;
        }
        if (codepoint == '\t') codepoint = ' ';
        cursor += kerning(previous, codepoint, pixelHeight);
        const GlyphBitmap& glyph = glyphFor(codepoint, static_cast<int>(std::lround(pixelHeight)));
        for (int row = 0; row < glyph.height; ++row) {
            for (int col = 0; col < glyph.width; ++col) {
                const std::uint8_t cover = glyph.coverage[static_cast<std::size_t>(row * glyph.width + col)];
                if (cover == 0) continue;
                Color ink = drawStyle.textColor;
                ink.a = static_cast<std::uint8_t>(std::lround(ink.a * (cover / 255.0)));
                pixel(static_cast<int>(std::floor(cursor)) + glyph.xoff + col,
                      static_cast<int>(std::floor(baseline)) + glyph.yoff + row,
                      ink, drawNode.opacity);
            }
        }
        cursor += glyph.advance;
        previous = codepoint;
    }
    currentBlendMode_ = previousBlend;
}

void SoftwareRenderer::drawParticles(const Node& node, const ComputedStyle& style,
                                     const Asset* texture) {
    const Color fallback = Color::parse(node.attributes.contains("color")
        ? node.attributes.at("color") : "").value_or(Color{255, 210, 80, 255});
    const Transform emitterTransform = node.worldTransform();
    constexpr double transformRadians = 3.14159265358979323846 / 180.0;
    const double emitterCos = std::cos(emitterTransform.rotation * transformRadians);
    const double emitterSin = std::sin(emitterTransform.rotation * transformRadians);
    for (const Particle& particle : node.emitter.particles) {
        const double progress = std::clamp(particle.age /
            std::max(0.001, particle.lifetime), 0.0, 1.0);
        const double size = particle.size + (particle.endSize-particle.size)*progress;
        Node sprite;
        sprite.type = NodeType::Sprite;
        if (particle.camera) {
            const double particleX = particle.position.x * emitterTransform.scale.x;
            const double particleY = particle.position.y * emitterTransform.scale.y;
            sprite.transform.position = {
                emitterTransform.position.x + particleX * emitterCos - particleY * emitterSin - size*particle.aspect*0.5,
                emitterTransform.position.y + particleX * emitterSin + particleY * emitterCos - size*0.5};
            sprite.transform.rotation = emitterTransform.rotation + particle.rotation;
        } else {
            sprite.transform.position = {particle.position.x - size*particle.aspect*0.5,
                                         particle.position.y - size*0.5};
            sprite.transform.rotation = particle.rotation;
        }
        sprite.width = size * particle.aspect;
        sprite.height = size;
        sprite.opacity = node.opacity;
        sprite.source = node.source;
        ComputedStyle particleStyle = style;
        const Color lifetimeColor=Color::lerp(particle.color.a?particle.color:fallback,
                                              particle.endColor,progress);
        particleStyle.fill=style.fill?std::optional<Paint>{modulatePaint(*style.fill,lifetimeColor)}
                                     :std::optional<Paint>{lifetimeColor};
        particleStyle.borderRadius = particle.shape == ParticleShape::Circle ? size*0.5 : 0.0;
        drawSprite(sprite, particleStyle, texture);
    }
}

void SoftwareRenderer::drawSpriteBatch(const Node& node, const ComputedStyle& style,
                                       const Asset* texture) {
    const Transform batchTransform = node.worldTransform();
    constexpr double transformRadians = 3.14159265358979323846 / 180.0;
    const double batchCos = std::cos(batchTransform.rotation * transformRadians);
    const double batchSin = std::sin(batchTransform.rotation * transformRadians);
    const double defaultWidth = node.width > 0 ? node.width :
        (texture && texture->width > 0 ? texture->width : 1.0);
    const double defaultHeight = node.height > 0 ? node.height :
        (texture && texture->height > 0 ? texture->height : 1.0);
    const bool hasTexture = texture && texture->available && texture->width > 0 &&
                            texture->height > 0 && !texture->pixels.empty();
    if (!hasTexture) {
        // Preserve styled placeholder behavior for textureless batches. Real
        // sprite fields take the optimized path below.
        for (const SpriteInstance& instance : node.batch.sprites) {
            if (!instance.visible || instance.opacity <= 0) continue;
            Node sprite;
            sprite.type = NodeType::Sprite;
            sprite.source = node.source;
            const double instanceX = instance.x * batchTransform.scale.x;
            const double instanceY = instance.y * batchTransform.scale.y;
            sprite.transform.position = {batchTransform.position.x + instanceX * batchCos - instanceY * batchSin,
                                         batchTransform.position.y + instanceX * batchSin + instanceY * batchCos};
            sprite.transform.rotation = batchTransform.rotation + instance.rotation;
            sprite.transform.scale = {batchTransform.scale.x * instance.scaleX,
                                      batchTransform.scale.y * instance.scaleY};
            sprite.width = instance.width > 0 ? instance.width : defaultWidth;
            sprite.height = instance.height > 0 ? instance.height : defaultHeight;
            sprite.opacity = node.opacity * instance.opacity;
            sprite.tint = modulateColor(node.tint, instance.tint);
            sprite.hue=node.hue;
            sprite.saturation=node.saturation;
            drawSprite(sprite, style, nullptr);
        }
        return;
    }

    // Large retained fields are overwhelmingly likely to be background/detail
    // layers. Cache them automatically so existing games do not need to opt in
    // before they benefit; their revision invalidates the raster if they move.
    constexpr std::size_t automaticCacheThreshold = 256;
    const bool cacheRequested = node.batch.sprites.size() >= automaticCacheThreshold ||
        (node.attributes.contains("cache") && boolean(node.attributes.at("cache"), false)) ||
        (node.attributes.contains("static") && boolean(node.attributes.at("static"), false));
    if (cacheRequested) {
        std::size_t signature = node.batch.revision;
        auto combine = [&](std::size_t value) {
            signature ^= value + 0x9e3779b97f4a7c15ull + (signature << 6) + (signature >> 2);
        };
        auto combineNumber = [&](double value) { combine(std::hash<double>{}(value)); };
        combine(std::hash<std::string>{}(node.id));
        combine(std::hash<std::string>{}(node.source));
        combine(reinterpret_cast<std::size_t>(texture));
        combineNumber(batchTransform.position.x); combineNumber(batchTransform.position.y);
        combineNumber(batchTransform.rotation); combineNumber(batchTransform.scale.x);
        combineNumber(batchTransform.scale.y); combineNumber(node.opacity);
        combineNumber(node.hue);combineNumber(node.saturation);
        combine(static_cast<std::size_t>(node.tint.r) << 24 |
                static_cast<std::size_t>(node.tint.g) << 16 |
                static_cast<std::size_t>(node.tint.b) << 8 | node.tint.a);
        // Both Wii U outputs render the same 640x480 logical canvas, so a
        // retained batch raster can be reused for TV and GamePad instead of
        // being built and stored twice.
        const std::string cacheKey = !node.id.empty() ? node.id :
            std::to_string(reinterpret_cast<std::size_t>(&node));
        BatchRasterCache& cache = batchRasterCache_[cacheKey];
        if (cache.signature != signature || cache.pixels.size() != currentPixels_->size()) {
            SoftwareRenderer raster(logicalWidth_, logicalHeight_, {}, renderScale_);
            raster.beginDisplay(Display::Tv, {0, 0, 0, 0});
            Node uncached;
            uncached.type = NodeType::SpriteBatch;
            uncached.id = node.id;
            uncached.source = node.source;
            uncached.transform = batchTransform;
            uncached.width = node.width;
            uncached.height = node.height;
            uncached.opacity = node.opacity;
            uncached.tint = node.tint;
            uncached.hue=node.hue;
            uncached.saturation=node.saturation;
            uncached.batch = node.batch;
            ComputedStyle normalStyle = style;
            normalStyle.blendMode = BlendMode::Normal;
            raster.drawSpriteBatch(uncached, normalStyle, texture);
            cache.pixels = raster.pixels(Display::Tv);
            cache.activePixels.clear();
            for (std::size_t index = 0; index < cache.pixels.size(); ++index)
                if (cache.pixels[index].a) cache.activePixels.push_back(
                    static_cast<std::uint32_t>(index));
            cache.signature = signature;
        }
        const BlendMode previousBlend = currentBlendMode_;
        currentBlendMode_ = style.blendMode;
        for (const std::uint32_t index : cache.activePixels)
            blendPixel((*currentPixels_)[index], cache.pixels[index], 1.0);
        currentBlendMode_ = previousBlend;
        return;
    }

    const BlendMode previousBlend = currentBlendMode_;
    currentBlendMode_ = style.blendMode;
    for (const SpriteInstance& instance : node.batch.sprites) {
        if (!instance.visible || instance.opacity <= 0) continue;
        const double instanceX = instance.x * batchTransform.scale.x;
        const double instanceY = instance.y * batchTransform.scale.y;
        const double left = (batchTransform.position.x + instanceX * batchCos -
                             instanceY * batchSin) * renderScale_;
        const double top = (batchTransform.position.y + instanceX * batchSin +
                            instanceY * batchCos) * renderScale_;
        const double spriteWidth = std::abs((instance.width > 0 ? instance.width : defaultWidth) *
            batchTransform.scale.x * instance.scaleX * renderScale_);
        const double spriteHeight = std::abs((instance.height > 0 ? instance.height : defaultHeight) *
            batchTransform.scale.y * instance.scaleY * renderScale_);
        if (spriteWidth < 0.01 || spriteHeight < 0.01) continue;
        const double angle = (batchTransform.rotation + instance.rotation) * transformRadians;
        const double cosAngle = std::cos(-angle), sinAngle = std::sin(-angle);
        const double centerX = left + spriteWidth * 0.5;
        const double centerY = top + spriteHeight * 0.5;
        const double extentX = std::abs(cosAngle) * spriteWidth * 0.5 +
                               std::abs(sinAngle) * spriteHeight * 0.5;
        const double extentY = std::abs(sinAngle) * spriteWidth * 0.5 +
                               std::abs(cosAngle) * spriteHeight * 0.5;
        const int x0 = std::max(0, static_cast<int>(std::floor(centerX - extentX)));
        const int x1 = std::min(width_, static_cast<int>(std::ceil(centerX + extentX)));
        const int y0 = std::max(0, static_cast<int>(std::floor(centerY - extentY)));
        const int y1 = std::min(height_, static_cast<int>(std::ceil(centerY + extentY)));
        if (x0 >= x1 || y0 >= y1) continue;
        const Color tint = modulateColor(node.tint, instance.tint);
        const double opacity = node.opacity * instance.opacity;

        if (std::abs(angle) < 0.00001) {
            for (int y = y0; y < y1; ++y) {
                const int sourceY = std::clamp(static_cast<int>((y + 0.5 - top) /
                    spriteHeight * texture->height), 0, texture->height - 1);
                const std::size_t sourceRow = static_cast<std::size_t>(sourceY * texture->width);
                const std::size_t destinationRow = static_cast<std::size_t>(y * width_);
                for (int x = x0; x < x1; ++x) {
                    const int sourceX = std::clamp(static_cast<int>((x + 0.5 - left) /
                        spriteWidth * texture->width), 0, texture->width - 1);
                    Color source = modulateColor(applyHueSaturation(texture->pixels[sourceRow +
                        static_cast<std::size_t>(sourceX)],node.hue,node.saturation), tint);
                    if (source.a == 0) continue;
                    blendPixel((*currentPixels_)[destinationRow + static_cast<std::size_t>(x)],
                               source, opacity);
                }
            }
            continue;
        }

        for (int y = y0; y < y1; ++y) {
            const std::size_t destinationRow = static_cast<std::size_t>(y * width_);
            for (int x = x0; x < x1; ++x) {
                const double dx = x + 0.5 - centerX;
                const double dy = y + 0.5 - centerY;
                const double localX = dx * cosAngle - dy * sinAngle + spriteWidth * 0.5;
                const double localY = dx * sinAngle + dy * cosAngle + spriteHeight * 0.5;
                if (localX < 0 || localY < 0 || localX >= spriteWidth || localY >= spriteHeight)
                    continue;
                const int sourceX = std::clamp(static_cast<int>(localX / spriteWidth * texture->width),
                                               0, texture->width - 1);
                const int sourceY = std::clamp(static_cast<int>(localY / spriteHeight * texture->height),
                                               0, texture->height - 1);
                Color source = modulateColor(applyHueSaturation(
                    texture->pixels[static_cast<std::size_t>(
                        sourceY * texture->width + sourceX)],node.hue,node.saturation), tint);
                if (source.a == 0) continue;
                blendPixel((*currentPixels_)[destinationRow + static_cast<std::size_t>(x)],
                           source, opacity);
            }
        }
    }
    currentBlendMode_ = previousBlend;
}

void SoftwareRenderer::drawShape(const Node& node, const ComputedStyle& style) {
    Node drawNode = scaledNode(node);
    ComputedStyle drawStyle = scaledStyle(style);
    const std::string kind = node.attributes.contains("kind")
        ? node.attributes.at("kind") : "rectangle";
    if (kind == "rectangle" || kind == "rect" || kind == "shape") {
        drawRectanglePixels(drawNode, drawStyle);
        return;
    }
    const BlendMode previousBlend = currentBlendMode_;
    currentBlendMode_ = drawStyle.blendMode;
    const Rect bounds = drawNode.bounds();
    auto paint = [&](double u, double v, int x, int y) {
        return drawStyle.fill ? paintAt(*drawStyle.fill, u, v, x, y) : drawStyle.borderColor;
    };
    auto attributeNumber = [&](const char* name, double fallback) {
        auto found = node.attributes.find(name);
        return found == node.attributes.end() ? fallback :
            cafe::number(found->second, fallback) * renderScale_;
    };
    if (kind == "circle" || kind == "ellipse") {
        const double rx = kind == "circle" ? std::min(bounds.width, bounds.height) * 0.5 : bounds.width * 0.5;
        const double ry = kind == "circle" ? rx : bounds.height * 0.5;
        const double cx = bounds.x + bounds.width * 0.5;
        const double cy = bounds.y + bounds.height * 0.5;
        const double stroke = std::max(0.0, drawStyle.borderWidth);
        for (int y = static_cast<int>(std::floor(cy - ry - stroke));
             y <= static_cast<int>(std::ceil(cy + ry + stroke)); ++y) {
            for (int x = static_cast<int>(std::floor(cx - rx - stroke));
                 x <= static_cast<int>(std::ceil(cx + rx + stroke)); ++x) {
                const double nx = (x + 0.5 - cx) / std::max(0.001, rx);
                const double ny = (y + 0.5 - cy) / std::max(0.001, ry);
                const double distance = std::hypot(nx, ny);
                if (distance <= 1.0) {
                    if (stroke > 0 && distance >= 1.0 - stroke / std::max(rx, ry))
                        pixel(x, y, drawStyle.borderColor, drawNode.opacity);
                    else pixel(x, y, paint(nx * 0.5 + 0.5, ny * 0.5 + 0.5, x, y), drawNode.opacity);
                }
            }
        }
    } else if (kind == "line") {
        const Vec2 start{bounds.x, bounds.y};
        const Vec2 end{drawNode.transform.position.x + attributeNumber("x2", node.width),
                       drawNode.transform.position.y + attributeNumber("y2", node.height)};
        const double width = std::max(1.0, drawStyle.borderWidth > 0 ?
            drawStyle.borderWidth : attributeNumber("stroke-width", 1));
        const double lengthSquared = std::max(0.0001,
            (end.x-start.x)*(end.x-start.x)+(end.y-start.y)*(end.y-start.y));
        const int x0 = static_cast<int>(std::floor(std::min(start.x,end.x)-width));
        const int x1 = static_cast<int>(std::ceil(std::max(start.x,end.x)+width));
        const int y0 = static_cast<int>(std::floor(std::min(start.y,end.y)-width));
        const int y1 = static_cast<int>(std::ceil(std::max(start.y,end.y)+width));
        for (int y=y0;y<=y1;++y) for(int x=x0;x<=x1;++x) {
            const double t=std::clamp(((x+.5-start.x)*(end.x-start.x)+(y+.5-start.y)*(end.y-start.y))/lengthSquared,0.0,1.0);
            const double px=start.x+(end.x-start.x)*t, py=start.y+(end.y-start.y)*t;
            if(std::hypot(x+.5-px,y+.5-py)<=width*.5)
                pixel(x,y,paint(t,0.5,x,y),drawNode.opacity);
        }
    } else if (kind == "polygon") {
        std::vector<Vec2> points;
        const std::string data = node.attributes.contains("points") ? node.attributes.at("points") : "";
        const char* cursor = data.c_str();
        while (*cursor) {
            char* end = nullptr; const double x = std::strtod(cursor, &end);
            if (end == cursor) { ++cursor; continue; } cursor = end;
            const double y = std::strtod(cursor, &end);
            if (end == cursor) break;
            cursor = end;
            points.push_back({drawNode.transform.position.x + x * renderScale_,
                              drawNode.transform.position.y + y * renderScale_});
        }
        if (points.size() >= 3) {
            double minX=points[0].x,maxX=minX,minY=points[0].y,maxY=minY;
            for(const Vec2 point:points){minX=std::min(minX,point.x);maxX=std::max(maxX,point.x);minY=std::min(minY,point.y);maxY=std::max(maxY,point.y);}
            for(int y=static_cast<int>(std::floor(minY));y<=static_cast<int>(std::ceil(maxY));++y)
                for(int x=static_cast<int>(std::floor(minX));x<=static_cast<int>(std::ceil(maxX));++x){
                    bool inside=false;for(std::size_t i=0,j=points.size()-1;i<points.size();j=i++){
                        const Vec2 a=points[i],b=points[j];if(((a.y>y+.5)!=(b.y>y+.5))&&
                            x+.5<(b.x-a.x)*(y+.5-a.y)/((b.y-a.y)==0?0.000001:b.y-a.y)+a.x)inside=!inside;}
                    if(inside)pixel(x,y,paint((x-minX)/std::max(1.0,maxX-minX),(y-minY)/std::max(1.0,maxY-minY),x,y),drawNode.opacity);
                }
        }
    } else if (kind == "arc") {
        constexpr double pi = 3.14159265358979323846;
        const double cx=bounds.x+bounds.width*.5,cy=bounds.y+bounds.height*.5;
        const double rx=bounds.width*.5,ry=bounds.height*.5;
        const double start=attributeNumber("start",0)/renderScale_;
        const double finish=attributeNumber("end",360)/renderScale_;
        const double stroke=std::max(1.0,drawStyle.borderWidth>0?drawStyle.borderWidth:attributeNumber("stroke-width",1));
        auto inRange=[&](double degrees){double a=std::fmod(degrees+360.0,360.0),s=std::fmod(start+360.0,360.0),e=std::fmod(finish+360.0,360.0);return s<=e?(a>=s&&a<=e):(a>=s||a<=e);};
        for(int y=static_cast<int>(std::floor(cy-ry-stroke));y<=static_cast<int>(std::ceil(cy+ry+stroke));++y)
            for(int x=static_cast<int>(std::floor(cx-rx-stroke));x<=static_cast<int>(std::ceil(cx+rx+stroke));++x){
                const double nx=(x+.5-cx)/std::max(.001,rx),ny=(y+.5-cy)/std::max(.001,ry);
                const double radial=std::hypot(nx,ny);const double angle=std::atan2(ny,nx)*180.0/pi;
                if(inRange(angle)&&std::abs(radial-1.0)<=stroke/std::max(rx,ry)*.5)
                    pixel(x,y,paint(nx*.5+.5,ny*.5+.5,x,y),drawNode.opacity);
            }
    }
    currentBlendMode_ = previousBlend;
}

void SoftwareRenderer::drawMesh3D(const Node& node, const ComputedStyle& style,
                                  const Asset* mesh, const Node* camera) {
    const BlendMode previousBlend = currentBlendMode_;
    currentBlendMode_ = style.blendMode;
    static const std::vector<MeshVertex> cubeVertices = {
        {{-0.5,-0.5,-0.5}}, {{0.5,-0.5,-0.5}}, {{0.5,0.5,-0.5}}, {{-0.5,0.5,-0.5}},
        {{-0.5,-0.5, 0.5}}, {{0.5,-0.5, 0.5}}, {{0.5,0.5, 0.5}}, {{-0.5,0.5, 0.5}}
    };
    static const std::vector<std::uint32_t> cubeIndices = {
        0,2,1, 0,3,2, 4,5,6, 4,6,7, 0,1,5, 0,5,4,
        3,7,6, 3,6,2, 1,2,6, 1,6,5, 0,4,7, 0,7,3
    };
    const auto& vertices = mesh && !mesh->vertices.empty() ? mesh->vertices : cubeVertices;
    const auto& indices = mesh && mesh->indices.size() >= 3 ? mesh->indices : cubeIndices;
    const Vec3 cameraPosition = camera ? camera->transform3D.position : Vec3{0, 0, -5};
    const Vec3 cameraRotation = camera ? camera->transform3D.rotation : Vec3{};
    const double nearClip = camera ? std::max(0.01, camera->nearClip) : 0.1;
    const double farClip = camera ? std::max(nearClip + 1.0, camera->farClip) : 1000.0;
    const double fov = camera ? std::clamp(camera->fieldOfView, 10.0, 150.0) : 60.0;
    const double focal = height_ * 0.5 / std::tan(fov * 3.14159265358979323846 / 360.0);
    const double sx = node.width > 0 ? node.width : node.transform3D.scale.x;
    const double sy = node.height > 0 ? node.height : node.transform3D.scale.y;
    const double sz = node.depth > 1.0 ? node.depth : node.transform3D.scale.z;
    const Vec3 lightDirection = normalized(Vec3{-0.4, 0.7, -0.6});
    struct Projected { double x, y, z; Vec3 world; double light; bool valid; };
    std::vector<Projected> projected;
    projected.reserve(vertices.size());
    for (const MeshVertex& vertex : vertices) {
        Vec3 local{vertex.position.x * sx, vertex.position.y * sy, vertex.position.z * sz};
        Vec3 world = rotateEuler(local, node.transform3D.rotation) + node.transform3D.position;
        Vec3 view = inverseRotateEuler(world - cameraPosition, cameraRotation);
        const bool valid = view.z > nearClip && view.z < farClip;
        Vec3 vertexNormal = vertex.normal;
        if (dot(vertexNormal,vertexNormal)<0.000001) vertexNormal=normalized(vertex.position);
        vertexNormal=normalized(rotateEuler(vertexNormal,node.transform3D.rotation));
        const double vertexLight=std::clamp(0.2+std::max(0.0,dot(vertexNormal,lightDirection))*0.8,0.2,1.0);
        projected.push_back({width_ * 0.5 + view.x / std::max(nearClip, view.z) * focal,
                             height_ * 0.5 - view.y / std::max(nearClip, view.z) * focal,
                             view.z, world, vertexLight, valid});
    }
    Color base{95, 185, 255, 255};
    if (style.fill) base = paintAt(*style.fill, 0.5, 0.5, 0, 0);
    for (std::size_t triangle = 0; triangle + 2 < indices.size(); triangle += 3) {
        const std::uint32_t ia = indices[triangle], ib = indices[triangle + 1], ic = indices[triangle + 2];
        if (ia >= projected.size() || ib >= projected.size() || ic >= projected.size()) continue;
        const Projected& a = projected[ia]; const Projected& b = projected[ib]; const Projected& c = projected[ic];
        if (!a.valid || !b.valid || !c.valid) continue;
        const double area = (b.x-a.x)*(c.y-a.y) - (b.y-a.y)*(c.x-a.x);
        // Screen-space Y points downward, so clockwise screen triangles are
        // front-facing. The previous sign rendered the rear faces of cubes.
        if (std::abs(area) < 0.0001 || area <= 0.0) continue;
        const int minX = std::max(0, static_cast<int>(std::floor(std::min({a.x,b.x,c.x}))));
        const int maxX = std::min(width_-1, static_cast<int>(std::ceil(std::max({a.x,b.x,c.x}))));
        const int minY = std::max(0, static_cast<int>(std::floor(std::min({a.y,b.y,c.y}))));
        const int maxY = std::min(height_-1, static_cast<int>(std::ceil(std::max({a.y,b.y,c.y}))));
        const Vec3 normal = normalized(cross(b.world-a.world, c.world-a.world));
        const double flatLight=style.shadingMode==ShadingMode::Unlit?1.0:
            0.25+std::max(0.0,dot(normal,lightDirection))*0.75;
        const Color shaded = litColor(base,flatLight);
        for (int y = minY; y <= maxY; ++y) for (int x = minX; x <= maxX; ++x) {
            const double px=x+0.5, py=y+0.5;
            const double w0=((b.x-px)*(c.y-py)-(b.y-py)*(c.x-px))/area;
            const double w1=((c.x-px)*(a.y-py)-(c.y-py)*(a.x-px))/area;
            const double w2=1.0-w0-w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            const double depth = 1.0 / (w0/a.z + w1/b.z + w2/c.z);
            const std::size_t offset = static_cast<std::size_t>(y*width_+x);
            if (depth >= depthBuffer_[offset]) continue;
            depthBuffer_[offset] = depth;
            const Color output=style.shadingMode==ShadingMode::Gouraud
                ?litColor(base,w0*a.light+w1*b.light+w2*c.light):shaded;
            pixel(x, y, output, node.opacity);
        }
    }
    currentBlendMode_ = previousBlend;
}

void SoftwareRenderer::writePpm(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::binary); if (!out) throw std::runtime_error("Cannot write preview: " + path.string());
    out << "P6\n" << width_ << ' ' << (height_ * 2) << "\n255\n";
    auto write = [&](const std::vector<Color>& buffer) {
        for (Color c : buffer) { out.put(static_cast<char>(c.r)); out.put(static_cast<char>(c.g)); out.put(static_cast<char>(c.b)); }
    };
    write(tvPixels_);
    write(gamePadPixels_);
}

void SoftwareRenderer::present() {
    if (output_.empty()) return;
    writePpm(output_);
}

} // namespace cafe
