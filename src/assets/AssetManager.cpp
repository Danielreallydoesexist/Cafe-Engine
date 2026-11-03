#include "cafe/assets/AssetManager.hpp"
#include "cafe/assets/Vorbis.hpp"
#include "cafe/core/File.hpp"
#include "ProceduralGenerator.hpp"

#include <png.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <unordered_map>

namespace cafe {
namespace {

std::string lowerExtension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}

bool decodePpm(const std::vector<std::uint8_t>& bytes, Asset& asset) {
    std::istringstream in(std::string(bytes.begin(), bytes.end()), std::ios::binary);
    std::string magic;
    int maximum = 0;
    in >> magic >> asset.width >> asset.height >> maximum;
    if (!in || asset.width <= 0 || asset.height <= 0 || maximum <= 0) return false;
    asset.pixels.resize(static_cast<std::size_t>(asset.width * asset.height));
    if (magic == "P3") {
        for (auto& pixel : asset.pixels) {
            int r = 0, g = 0, b = 0;
            in >> r >> g >> b;
            if (!in) return false;
            pixel = {static_cast<std::uint8_t>(r * 255 / maximum),
                     static_cast<std::uint8_t>(g * 255 / maximum),
                     static_cast<std::uint8_t>(b * 255 / maximum), 255};
        }
        return true;
    }
    if (magic != "P6") return false;
    in.get();
    for (auto& pixel : asset.pixels) {
        char rgb[3]{};
        in.read(rgb, 3);
        if (!in) return false;
        pixel = {static_cast<std::uint8_t>(rgb[0]), static_cast<std::uint8_t>(rgb[1]),
                 static_cast<std::uint8_t>(rgb[2]), 255};
    }
    return true;
}

bool decodePng(const std::vector<std::uint8_t>& bytes, Asset& asset) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size())) return false;
    image.format = PNG_FORMAT_RGBA;
    std::vector<std::uint8_t> rgba(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, rgba.data(), 0, nullptr)) {
        png_image_free(&image);
        return false;
    }
    asset.width = static_cast<int>(image.width);
    asset.height = static_cast<int>(image.height);
    asset.pixels.resize(static_cast<std::size_t>(asset.width * asset.height));
    std::memcpy(asset.pixels.data(), rgba.data(), rgba.size());
    png_image_free(&image);
    return true;
}

// PNG encoders commonly leave fully transparent texels black. Bilinear GPU
// sampling then mixes that black into the last visible texel (notably around
// An EgG's eggs). Fill only invisible RGB from adjacent colored texels; alpha
// and every visible pixel stay exactly as authored. This runs once on load.
void bleedTransparentEdgeRgb(Asset& asset) {
    if (asset.width < 2 || asset.height < 2) return;
    const int width = asset.width, height = asset.height;
    {
        const auto previous = asset.pixels;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * width + x;
                if (previous[index].a != 0) continue;
                int totalR = 0, totalG = 0, totalB = 0, count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int px = x + dx, py = y + dy;
                        if ((dx == 0 && dy == 0) || px < 0 || py < 0 ||
                            px >= width || py >= height) continue;
                        const Color& source = previous[static_cast<std::size_t>(py) * width + px];
                        if (source.a == 0) continue;
                        totalR += source.r; totalG += source.g; totalB += source.b;
                        ++count;
                    }
                }
                if (count) {
                    Color& pixel = asset.pixels[index];
                    pixel.r = static_cast<std::uint8_t>(totalR / count);
                    pixel.g = static_cast<std::uint8_t>(totalG / count);
                    pixel.b = static_cast<std::uint8_t>(totalB / count);
                }
            }
        }
    }
}

std::string attribute(std::string_view tag, std::string_view name) {
    std::size_t position = 0;
    while ((position = tag.find(name, position)) != std::string_view::npos) {
        const bool leftOk = position == 0 || std::isspace(static_cast<unsigned char>(tag[position - 1]));
        std::size_t cursor = position + name.size();
        while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor]))) ++cursor;
        if (!leftOk || cursor >= tag.size() || tag[cursor] != '=') { position = cursor; continue; }
        ++cursor;
        while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor]))) ++cursor;
        if (cursor >= tag.size() || (tag[cursor] != '\'' && tag[cursor] != '"')) return {};
        const char quote = tag[cursor++];
        const std::size_t end = tag.find(quote, cursor);
        return end == std::string_view::npos ? std::string{} : std::string(tag.substr(cursor, end - cursor));
    }
    return {};
}

std::string styleValue(std::string_view tag, std::string_view name) {
    std::string direct = attribute(tag, name);
    if (!direct.empty()) return direct;
    const std::string style = attribute(tag, "style");
    std::size_t position = 0;
    while (position < style.size()) {
        const std::size_t colon = style.find(':', position);
        if (colon == std::string::npos) break;
        const std::size_t end = style.find(';', colon + 1);
        if (trim(std::string_view(style).substr(position, colon - position)) == name)
            return trim(std::string_view(style).substr(colon + 1, end - colon - 1));
        position = end == std::string::npos ? style.size() : end + 1;
    }
    return {};
}

Color svgColor(std::string value, Color fallback = {255, 255, 255, 255}) {
    value = trim(value);
    if (value.empty()) return fallback;
    if (value == "none") return {0, 0, 0, 0};
    if (auto color = Color::parse(value)) return *color;
    if (value == "black") return {0, 0, 0, 255};
    if (value == "white") return {255, 255, 255, 255};
    if (value == "red") return {255, 0, 0, 255};
    if (value == "green") return {0, 128, 0, 255};
    if (value == "blue") return {0, 0, 255, 255};
    if (value == "transparent") return {0, 0, 0, 0};
    return fallback;
}

void blend(Color& destination, Color source) {
    if (source.a == 255) { destination = source; return; }
    const int inverse = 255 - source.a;
    destination.r = static_cast<std::uint8_t>((source.r * source.a + destination.r * inverse) / 255);
    destination.g = static_cast<std::uint8_t>((source.g * source.a + destination.g * inverse) / 255);
    destination.b = static_cast<std::uint8_t>((source.b * source.a + destination.b * inverse) / 255);
    destination.a = static_cast<std::uint8_t>(source.a + destination.a * inverse / 255);
}

void svgPixel(Asset& asset, int x, int y, Color color) {
    if (x < 0 || y < 0 || x >= asset.width || y >= asset.height || color.a == 0) return;
    blend(asset.pixels[static_cast<std::size_t>(y * asset.width + x)], color);
}

void fillPolygon(Asset& asset, const std::vector<Vec2>& points, Color color) {
    if (points.size() < 3 || color.a == 0) return;
    double minX = points[0].x, maxX = minX, minY = points[0].y, maxY = minY;
    for (Vec2 point : points) {
        minX = std::min(minX, point.x); maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y); maxY = std::max(maxY, point.y);
    }
    const int x0 = std::max(0, static_cast<int>(std::floor(minX)));
    const int x1 = std::min(asset.width, static_cast<int>(std::ceil(maxX)));
    const int y0 = std::max(0, static_cast<int>(std::floor(minY)));
    const int y1 = std::min(asset.height, static_cast<int>(std::ceil(maxY)));
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) {
        const double px = x + 0.5, py = y + 0.5;
        bool inside = false;
        for (std::size_t i = 0, j = points.size() - 1; i < points.size(); j = i++) {
            const Vec2 a = points[i], b = points[j];
            if (((a.y > py) != (b.y > py)) &&
                px < (b.x - a.x) * (py - a.y) / ((b.y - a.y) == 0 ? 0.0000001 : b.y - a.y) + a.x)
                inside = !inside;
        }
        if (inside) svgPixel(asset, x, y, color);
    }
}

void drawLine(Asset& asset, Vec2 a, Vec2 b, Color color, double width) {
    const double length = std::max(1.0, std::hypot(b.x - a.x, b.y - a.y));
    const int steps = static_cast<int>(std::ceil(length * 2.0));
    const int radius = std::max(0, static_cast<int>(std::ceil(width * 0.5)));
    for (int step = 0; step <= steps; ++step) {
        const double t = static_cast<double>(step) / steps;
        const int cx = static_cast<int>(std::lround(a.x + (b.x - a.x) * t));
        const int cy = static_cast<int>(std::lround(a.y + (b.y - a.y) * t));
        for (int y = cy - radius; y <= cy + radius; ++y)
            for (int x = cx - radius; x <= cx + radius; ++x)
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius + 1)
                    svgPixel(asset, x, y, color);
    }
}

std::vector<double> svgNumbers(std::string_view value) {
    std::vector<double> numbers;
    const char* cursor = value.data();
    const char* end = cursor + value.size();
    while (cursor < end) {
        while (cursor < end && !(std::isdigit(static_cast<unsigned char>(*cursor)) ||
               *cursor == '-' || *cursor == '+' || *cursor == '.')) ++cursor;
        if (cursor >= end) break;
        char* next = nullptr;
        numbers.push_back(std::strtod(cursor, &next));
        if (next == cursor) break;
        cursor = next;
    }
    return numbers;
}

std::vector<std::vector<Vec2>> flattenPath(std::string_view data, double sx, double sy,
                                           double ox, double oy) {
    std::vector<std::vector<Vec2>> paths;
    std::vector<Vec2>* path = nullptr;
    std::size_t position = 0;
    char command = 0;
    Vec2 current{}, start{};
    auto numberAt = [&](double& value) {
        while (position < data.size() && (std::isspace(static_cast<unsigned char>(data[position])) || data[position] == ',')) ++position;
        if (position >= data.size() || std::isalpha(static_cast<unsigned char>(data[position]))) return false;
        char* end = nullptr;
        value = std::strtod(data.data() + position, &end);
        if (end == data.data() + position) return false;
        position = static_cast<std::size_t>(end - data.data());
        return true;
    };
    auto mapped = [&](Vec2 point) { return Vec2{(point.x - ox) * sx, (point.y - oy) * sy}; };
    while (position < data.size()) {
        while (position < data.size() && (std::isspace(static_cast<unsigned char>(data[position])) || data[position] == ',')) ++position;
        if (position >= data.size()) break;
        if (std::isalpha(static_cast<unsigned char>(data[position]))) command = data[position++];
        if (!command) break;
        const bool relative = std::islower(static_cast<unsigned char>(command));
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(command)));
        if (upper == 'Z') {
            if (path && !path->empty()) path->push_back(mapped(start));
            current = start; command = 0; continue;
        }
        double values[6]{};
        const int needed = upper == 'H' || upper == 'V' ? 1 :
                           upper == 'M' || upper == 'L' || upper == 'T' ? 2 :
                           upper == 'Q' || upper == 'S' ? 4 : upper == 'C' ? 6 : 0;
        if (needed == 0) { ++position; continue; }
        bool complete = true;
        for (int i = 0; i < needed; ++i) if (!numberAt(values[i])) { complete = false; break; }
        if (!complete) { command = 0; continue; }
        if (upper == 'H') {
            current.x = relative ? current.x + values[0] : values[0];
            if (path) path->push_back(mapped(current));
        } else if (upper == 'V') {
            current.y = relative ? current.y + values[0] : values[0];
            if (path) path->push_back(mapped(current));
        } else if (upper == 'M' || upper == 'L' || upper == 'T') {
            Vec2 next{relative ? current.x + values[0] : values[0],
                      relative ? current.y + values[1] : values[1]};
            if (upper == 'M') {
                paths.emplace_back(); path = &paths.back(); start = next;
                command = relative ? 'l' : 'L';
            }
            current = next;
            if (path) path->push_back(mapped(current));
        } else {
            if (!path) { paths.emplace_back(); path = &paths.back(); path->push_back(mapped(current)); }
            const Vec2 origin = current;
            if (upper == 'Q' || upper == 'S') {
                Vec2 control{relative ? origin.x + values[0] : values[0], relative ? origin.y + values[1] : values[1]};
                Vec2 endPoint{relative ? origin.x + values[2] : values[2], relative ? origin.y + values[3] : values[3]};
                for (int step = 1; step <= 12; ++step) {
                    const double t = step / 12.0, u = 1.0 - t;
                    path->push_back(mapped(origin * (u*u) + control * (2*u*t) + endPoint * (t*t)));
                }
                current = endPoint;
            } else {
                Vec2 c1{relative ? origin.x + values[0] : values[0], relative ? origin.y + values[1] : values[1]};
                Vec2 c2{relative ? origin.x + values[2] : values[2], relative ? origin.y + values[3] : values[3]};
                Vec2 endPoint{relative ? origin.x + values[4] : values[4], relative ? origin.y + values[5] : values[5]};
                for (int step = 1; step <= 16; ++step) {
                    const double t = step / 16.0, u = 1.0 - t;
                    path->push_back(mapped(origin * (u*u*u) + c1 * (3*u*u*t) + c2 * (3*u*t*t) + endPoint * (t*t*t)));
                }
                current = endPoint;
            }
        }
    }
    return paths;
}

bool decodeSvg(const std::vector<std::uint8_t>& bytes, Asset& asset) {
    const std::string source(bytes.begin(), bytes.end());
    const std::size_t svgStart = source.find("<svg");
    if (svgStart == std::string::npos) return false;
    const std::size_t svgEnd = source.find('>', svgStart);
    if (svgEnd == std::string::npos) return false;
    const std::string_view root(source.data() + svgStart, svgEnd - svgStart + 1);
    const auto dimensions = svgNumbers(attribute(root, "viewBox"));
    const double viewX = dimensions.size() >= 4 ? dimensions[0] : 0.0;
    const double viewY = dimensions.size() >= 4 ? dimensions[1] : 0.0;
    const double viewWidth = dimensions.size() >= 4 ? dimensions[2] : number(attribute(root, "width"), 256);
    const double viewHeight = dimensions.size() >= 4 ? dimensions[3] : number(attribute(root, "height"), 256);
    asset.width = std::max(1, static_cast<int>(std::lround(number(attribute(root, "width"), viewWidth))));
    asset.height = std::max(1, static_cast<int>(std::lround(number(attribute(root, "height"), viewHeight))));
    asset.pixels.assign(static_cast<std::size_t>(asset.width * asset.height), Color{0, 0, 0, 0});
    const double sx = asset.width / std::max(1.0, viewWidth);
    const double sy = asset.height / std::max(1.0, viewHeight);

    std::size_t position = svgEnd + 1;
    while ((position = source.find('<', position)) != std::string::npos) {
        const std::size_t end = source.find('>', position + 1);
        if (end == std::string::npos) break;
        const std::string_view tag(source.data() + position, end - position + 1);
        position = end + 1;
        if (tag.size() < 2 || tag[1] == '/' || tag[1] == '!' || tag[1] == '?') continue;
        std::size_t nameEnd = 1;
        while (nameEnd < tag.size() && std::isalpha(static_cast<unsigned char>(tag[nameEnd]))) ++nameEnd;
        const std::string name(tag.substr(1, nameEnd - 1));
        const Color fill = svgColor(styleValue(tag, "fill"), {0, 0, 0, 255});
        const Color stroke = svgColor(styleValue(tag, "stroke"), {0, 0, 0, 0});
        const double strokeWidth = number(styleValue(tag, "stroke-width"), 1.0) * (sx + sy) * 0.5;
        if (name == "rect") {
            const double x = (number(attribute(tag, "x")) - viewX) * sx;
            const double y = (number(attribute(tag, "y")) - viewY) * sy;
            const double width = number(attribute(tag, "width")) * sx;
            const double height = number(attribute(tag, "height")) * sy;
            fillPolygon(asset, {{x,y},{x+width,y},{x+width,y+height},{x,y+height}}, fill);
        } else if (name == "circle" || name == "ellipse") {
            const double cx = (number(attribute(tag, "cx")) - viewX) * sx;
            const double cy = (number(attribute(tag, "cy")) - viewY) * sy;
            const double rx = number(attribute(tag, name == "circle" ? "r" : "rx")) * sx;
            const double ry = number(attribute(tag, name == "circle" ? "r" : "ry")) * sy;
            std::vector<Vec2> points;
            for (int i = 0; i < 48; ++i) {
                const double angle = i * 6.283185307179586 / 48.0;
                points.push_back({cx + std::cos(angle) * rx, cy + std::sin(angle) * ry});
            }
            fillPolygon(asset, points, fill);
        } else if (name == "line") {
            drawLine(asset, {(number(attribute(tag,"x1"))-viewX)*sx, (number(attribute(tag,"y1"))-viewY)*sy},
                     {(number(attribute(tag,"x2"))-viewX)*sx, (number(attribute(tag,"y2"))-viewY)*sy}, stroke, strokeWidth);
        } else if (name == "polygon" || name == "polyline") {
            const auto values = svgNumbers(attribute(tag, "points"));
            std::vector<Vec2> points;
            for (std::size_t i = 1; i < values.size(); i += 2)
                points.push_back({(values[i-1]-viewX)*sx, (values[i]-viewY)*sy});
            if (name == "polygon") fillPolygon(asset, points, fill);
            if (stroke.a) for (std::size_t i = 1; i < points.size(); ++i) drawLine(asset, points[i-1], points[i], stroke, strokeWidth);
        } else if (name == "path") {
            for (const auto& points : flattenPath(attribute(tag, "d"), sx, sy, viewX, viewY)) {
                fillPolygon(asset, points, fill);
                if (stroke.a) for (std::size_t i = 1; i < points.size(); ++i)
                    drawLine(asset, points[i-1], points[i], stroke, strokeWidth);
            }
        }
    }
    return true;
}

std::uint16_t read16(const std::uint8_t* value) {
    return static_cast<std::uint16_t>(value[0] | (value[1] << 8));
}
std::uint32_t read32(const std::uint8_t* value) {
    return static_cast<std::uint32_t>(value[0] | (value[1] << 8) |
                                     (value[2] << 16) | (value[3] << 24));
}

bool decodeWav(const std::vector<std::uint8_t>& bytes, Asset& asset) {
    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) return false;
    std::uint16_t format = 0, bits = 0;
    const std::uint8_t* audio = nullptr;
    std::size_t audioSize = 0;
    for (std::size_t offset = 12; offset + 8 <= bytes.size();) {
        const std::uint32_t size = read32(bytes.data() + offset + 4);
        const std::size_t next = offset + 8 + size + (size & 1u);
        if (next > bytes.size() + 1) break;
        if (std::memcmp(bytes.data() + offset, "fmt ", 4) == 0 && size >= 16) {
            format = read16(bytes.data() + offset + 8);
            asset.channels = read16(bytes.data() + offset + 10);
            asset.sampleRate = static_cast<int>(read32(bytes.data() + offset + 12));
            bits = read16(bytes.data() + offset + 22);
        } else if (std::memcmp(bytes.data() + offset, "data", 4) == 0) {
            audio = bytes.data() + offset + 8;
            audioSize = std::min<std::size_t>(size, bytes.size() - offset - 8);
        }
        offset = next;
    }
    if (!audio || asset.channels <= 0 || asset.sampleRate <= 0) return false;
    if (format == 1 && bits == 16) {
        asset.samples.resize(audioSize / 2);
        for (std::size_t i = 0; i < asset.samples.size(); ++i)
            asset.samples[i] = static_cast<std::int16_t>(read16(audio + i * 2));
        return true;
    }
    if (format == 1 && bits == 8) {
        asset.samples.resize(audioSize);
        for (std::size_t i = 0; i < audioSize; ++i)
            asset.samples[i] = static_cast<std::int16_t>((static_cast<int>(audio[i]) - 128) << 8);
        return true;
    }
    if (format == 3 && bits == 32) {
        asset.samples.resize(audioSize / 4);
        for (std::size_t i = 0; i < asset.samples.size(); ++i) {
            float sample = 0;
            std::memcpy(&sample, audio + i * 4, 4);
            asset.samples[i] = static_cast<std::int16_t>(std::clamp(sample, -1.0f, 1.0f) * 32767);
        }
        return true;
    }
    return false;
}

bool decodeObj(const std::vector<std::uint8_t>& bytes, Asset& asset) {
    const std::string source(bytes.begin(), bytes.end());
    bool declaresSmoothing = false;
    {
        std::istringstream scan(source);
        for (std::string row; std::getline(scan, row);) {
            std::istringstream fields(row); std::string kind; fields >> kind;
            if (kind == "s") { declaresSmoothing = true; break; }
        }
    }

    std::istringstream input(source);
    std::vector<Vec3> positions, normals;
    std::vector<Vec2> uvs;
    struct FaceVertex { MeshVertex vertex; std::size_t position{0}; bool hasNormal{false}; };
    struct GeneratedNormal { std::size_t vertex{0}; std::string key; };
    std::vector<GeneratedNormal> generated;
    std::unordered_map<std::string, Vec3> normalSums;
    std::string smoothingGroup = declaresSmoothing ? std::string{} : std::string{"auto"};

    std::string line;
    while (std::getline(input, line)) {
        std::istringstream stream(line);
        std::string kind;
        stream >> kind;
        if (kind == "v") { Vec3 value; stream >> value.x >> value.y >> value.z; positions.push_back(value); }
        else if (kind == "vn") { Vec3 value; stream >> value.x >> value.y >> value.z; normals.push_back(value); }
        else if (kind == "vt") { Vec2 value; stream >> value.x >> value.y; uvs.push_back(value); }
        else if (kind == "s") {
            std::string group; stream >> group;
            smoothingGroup = (group == "off" || group == "0") ? std::string{} : group;
        } else if (kind == "f") {
            std::vector<FaceVertex> face;
            for (std::string token; stream >> token;) {
                int values[3]{0, 0, 0};
                std::size_t part = 0, start = 0;
                for (std::size_t i = 0; i <= token.size() && part < 3; ++i) if (i == token.size() || token[i] == '/') {
                    if (i > start) values[part] = std::stoi(token.substr(start, i - start));
                    ++part; start = i + 1;
                }
                auto index = [](int value, std::size_t size) -> std::size_t {
                    return value < 0 ? size - static_cast<std::size_t>(-value) :
                                       static_cast<std::size_t>(std::max(1, value) - 1);
                };
                FaceVertex value;
                if (values[0] && index(values[0], positions.size()) < positions.size()) {
                    value.position = index(values[0], positions.size());
                    value.vertex.position = positions[value.position];
                }
                if (values[1] && index(values[1], uvs.size()) < uvs.size())
                    value.vertex.uv = uvs[index(values[1], uvs.size())];
                if (values[2] && index(values[2], normals.size()) < normals.size()) {
                    value.vertex.normal = normalized(normals[index(values[2], normals.size())]);
                    value.hasNormal = true;
                }
                face.push_back(value);
            }
            for (std::size_t i = 1; i + 1 < face.size(); ++i) {
                FaceVertex triangle[3]{face[0], face[i], face[i + 1]};
                const Vec3 weighted = cross(triangle[1].vertex.position - triangle[0].vertex.position,
                                            triangle[2].vertex.position - triangle[0].vertex.position);
                const Vec3 faceNormal = normalized(weighted);
                for (FaceVertex& value : triangle) {
                    MeshVertex vertex = value.vertex;
                    if (!value.hasNormal && smoothingGroup.empty()) vertex.normal = faceNormal;
                    asset.indices.push_back(static_cast<std::uint32_t>(asset.vertices.size()));
                    asset.vertices.push_back(vertex);
                    if (!value.hasNormal && !smoothingGroup.empty()) {
                        const std::string key = smoothingGroup + ":" + std::to_string(value.position);
                        generated.push_back({asset.vertices.size() - 1, key});
                        auto& sum = normalSums[key];
                        sum = sum + weighted;
                    }
                }
            }
        }
    }
    for (const GeneratedNormal& entry : generated)
        asset.vertices[entry.vertex].normal = normalized(normalSums[entry.key]);
    return !asset.indices.empty();
}

} // namespace

const Asset& AssetManager::loadProcedural(std::string_view source) {
    const std::string key(source);
    if (auto found=proceduralIndex_.find(key);found!=proceduralIndex_.end()) {
        ProceduralSlot& slot=*proceduralSlots_[found->second];
        slot.lastUsed=++proceduralClock_;
        return slot.asset;
    }
    if (auto invalid=assets_.find(key);invalid!=assets_.end())
        return invalid->second;
    Asset generated{};
    if (!generateProceduralImage(source,generated)) {
        generated.type=AssetType::Image;
        return assets_.emplace(key,std::move(generated)).first->second;
    }
    std::size_t index=0;
    if (proceduralSlots_.size()<maxProceduralSlots) {
        index=proceduralSlots_.size();
        proceduralSlots_.push_back(std::make_unique<ProceduralSlot>());
    } else {
        for (std::size_t i=1;i<proceduralSlots_.size();++i)
            if (proceduralSlots_[i]->lastUsed<proceduralSlots_[index]->lastUsed) index=i;
        proceduralIndex_.erase(proceduralSlots_[index]->source);
    }
    ProceduralSlot& slot=*proceduralSlots_[index];
    const std::uint64_t revision=slot.asset.revision+1;
    slot.asset=std::move(generated);
    slot.asset.revision=revision;
    slot.source=key;
    slot.lastUsed=++proceduralClock_;
    proceduralIndex_.emplace(key,index);
    return slot.asset;
}

const Asset& AssetManager::load(std::string_view path, AssetType type) {
    if (type==AssetType::Image && path.starts_with("procedural:"))
        return loadProcedural(path);
    std::string key(path);
    if (auto found = assets_.find(key); found != assets_.end()) return found->second;
    const auto resolved = root_ / std::filesystem::path(key);
    Asset asset{};
    asset.type = type;
    asset.path = resolved;
    asset.available = fileReadable(resolved);
    if (asset.available) {
        const auto bytes = readBinaryFile(resolved);
        const std::string extension = lowerExtension(resolved);
        bool decoded = type == AssetType::Font;
        if (type == AssetType::Font) asset.bytes = bytes;
        if (type == AssetType::Image) {
            if (extension == ".ppm") decoded = decodePpm(bytes, asset);
            else if (extension == ".png") {
                decoded = decodePng(bytes, asset);
                if (decoded && resolved.filename() == "egg.png")
                    bleedTransparentEdgeRgb(asset);
            }
            else if (extension == ".svg") decoded = decodeSvg(bytes, asset);
        } else if (type == AssetType::Audio) {
            if (extension == ".wav") decoded = decodeWav(bytes, asset);
            else if (extension == ".ogg") decoded = decodeVorbis(bytes, asset.sampleRate,
                                                                   asset.channels, asset.samples);
        } else if (type == AssetType::Mesh && extension == ".obj") {
            decoded = decodeObj(bytes, asset);
        }
        asset.available = decoded;
    }
    return assets_.emplace(key, std::move(asset)).first->second;
}

const Asset& AssetManager::loadImage(std::string_view path) { return load(path, AssetType::Image); }
const Asset& AssetManager::loadFont(std::string_view path) { return load(path, AssetType::Font); }
const Asset& AssetManager::loadAudio(std::string_view path) { return load(path, AssetType::Audio); }
const Asset& AssetManager::loadMesh(std::string_view path) { return load(path, AssetType::Mesh); }
std::vector<MaskEdgePoint> AssetManager::edgePoints(std::string_view path,int spacing,
                                                   double threshold) {
    const Asset& asset=loadImage(path);
    if (!asset.available || asset.width<=0 || asset.height<=0) return {};
    spacing=std::clamp(spacing,4,128);
    threshold=std::clamp(threshold,0.0,1.0);
    EdgeCacheEntry& entry=edgeCache_[&asset];
    if (entry.revision!=asset.revision || entry.spacing!=spacing ||
        entry.threshold!=threshold) {
        entry.points=extractMaskEdgePoints(asset,spacing,threshold);
        entry.revision=asset.revision;
        entry.spacing=spacing;
        entry.threshold=threshold;
    }
    return entry.points;
}
const Asset* AssetManager::get(std::string_view path) const {
    if (path.starts_with("procedural:")) {
        auto procedural=proceduralIndex_.find(std::string(path));
        if (procedural!=proceduralIndex_.end())
            return &proceduralSlots_[procedural->second]->asset;
    }
    auto it = assets_.find(std::string(path));
    return it == assets_.end() ? nullptr : &it->second;
}

} // namespace cafe
