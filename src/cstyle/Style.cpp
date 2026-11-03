#include "cafe/cstyle/Style.hpp"
#include "cafe/core/File.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cafe {
namespace {

std::vector<std::string> splitTop(std::string_view source, char delimiter = ',') {
    std::vector<std::string> out; int depth = 0; std::size_t start = 0;
    for (std::size_t i = 0; i < source.size(); ++i) {
        if (source[i] == '(') ++depth; else if (source[i] == ')') --depth;
        else if (source[i] == delimiter && depth == 0) { out.push_back(trim(source.substr(start, i - start))); start = i + 1; }
    }
    if (start < source.size()) out.push_back(trim(source.substr(start)));
    return out;
}

Color namedColor(std::string_view name) {
    if (name == "red") return {255, 0, 0, 255};
    if (name == "blue") return {0, 80, 255, 255};
    if (name == "white") return {255, 255, 255, 255};
    if (name == "black") return {0, 0, 0, 255};
    if (name == "transparent") return {0, 0, 0, 0};
    return {};
}

Color color(std::string_view text) {
    const std::string value = trim(text);
    if (auto c = Color::parse(value)) return *c;
    if (value.starts_with("rgb(") || value.starts_with("rgba(")) {
        const auto open = value.find('('), close = value.rfind(')');
        if (open != std::string::npos && close != std::string::npos) {
            std::string fields = value.substr(open + 1, close - open - 1);
            std::replace(fields.begin(), fields.end(), ',', ' ');
            std::istringstream in(fields);
            double r=0,g=0,b=0,a=255;
            if (in >> r >> g >> b) {
                if (value.starts_with("rgba") && in >> a && a <= 1.0) a *= 255.0;
                return {static_cast<std::uint8_t>(std::clamp(std::lround(r),0l,255l)),
                        static_cast<std::uint8_t>(std::clamp(std::lround(g),0l,255l)),
                        static_cast<std::uint8_t>(std::clamp(std::lround(b),0l,255l)),
                        static_cast<std::uint8_t>(std::clamp(std::lround(a),0l,255l))};
            }
        }
    }
    return namedColor(value);
}

double dimension(std::string_view text, double fallback = 0) {
    std::string value = trim(text); if (value.ends_with("px")) value.resize(value.size() - 2);
    if (value.ends_with("deg")) value.resize(value.size() - 3);
    return number(value, fallback);
}

std::optional<UiState> state(std::string_view name) {
    if (name == "hover") return UiState::Hover;
    if (name == "pressed") return UiState::Pressed;
    if (name == "focus") return UiState::Focused;
    if (name == "disabled") return UiState::Disabled;
    return std::nullopt;
}

std::vector<ColorStop> stops(const std::vector<std::string>& pieces, std::size_t start) {
    std::vector<ColorStop> out;
    for (std::size_t i = start; i < pieces.size(); ++i) {
        std::istringstream in(pieces[i]); std::string c, pos; in >> c >> pos;
        out.push_back({pos.empty() ? -1 : dimension(pos) / (pos.ends_with('%') ? 100.0 : 1.0), color(c)});
    }
    for (std::size_t i = 0; i < out.size(); ++i) if (out[i].position < 0) out[i].position = out.size() == 1 ? 0 : static_cast<double>(i) / (out.size() - 1);
    return out;
}

std::string inner(std::string_view value) {
    auto a = value.find('('), b = value.rfind(')'); return a == std::string_view::npos || b == std::string_view::npos ? "" : trim(value.substr(a + 1, b - a - 1));
}

Selector parseSelector(std::string value) {
    value = trim(value); Selector result;
    if (auto colon = value.find(':'); colon != std::string::npos) { result.state = state(value.substr(colon + 1)); value.resize(colon); }
    if (!value.empty() && value.front() == '#') { result.kind = Selector::Kind::Id; value.erase(0, 1); }
    else if (!value.empty() && value.front() == '.') { result.kind = Selector::Kind::Class; value.erase(0, 1); }
    else result.kind = Selector::Kind::Element;
    result.name = trim(value); return result;
}

void applyDeclaration(ComputedStyle& style, std::string_view key, std::string_view raw) {
    std::string value = trim(raw);
    if (key == "fill") style.fill = parsePaint(value);
    else if (key == "background") { style.backgrounds.clear(); for (auto& part : splitTop(value)) if (auto p = parsePaint(part)) style.backgrounds.push_back(*p); }
    else if (key == "border-width") style.borderWidth = dimension(value);
    else if (key == "border-color") style.borderColor = color(value);
    else if (key == "border-radius") style.borderRadius = dimension(value);
    else if (key == "border") { std::istringstream in(value); std::string w, kind, c; in >> w >> kind >> c; style.borderWidth = dimension(w); style.borderColor = color(c); }
    else if (key == "color") style.textColor = color(value);
    else if (key == "font-size") style.fontSize = dimension(value, 20);
    else if (key == "font-source" || key == "font-family") {
        style.fontSource = trim(value);
        if (style.fontSource.starts_with("url(") && style.fontSource.ends_with(')'))
            style.fontSource = trim(std::string_view(style.fontSource).substr(4, style.fontSource.size() - 5));
        if (style.fontSource.size() >= 2 &&
            ((style.fontSource.front() == '"' && style.fontSource.back() == '"') ||
             (style.fontSource.front() == '\'' && style.fontSource.back() == '\'')))
            style.fontSource = style.fontSource.substr(1, style.fontSource.size() - 2);
    }
    else if (key == "blend-mode" || key == "mix-blend-mode") style.blendMode = parseBlendMode(value);
    else if (key == "shading" || key == "shading-mode") style.shadingMode = parseShadingMode(value);
    else if (key == "outer-shadow" || key == "inset-shadow") {
        std::istringstream in(value); std::string x, y, blur, c; in >> x >> y >> blur >> c;
        style.shadows.push_back({dimension(x), dimension(y), dimension(blur), color(c), key == "inset-shadow"});
    } else if (key == "sawtooth-edge") {
        std::istringstream in(value); Sawtooth s; std::string tooth, depth; in >> s.side >> tooth >> depth;
        s.tooth = dimension(tooth, 14); s.depth = dimension(depth, 8); style.sawtooth = s;
    }
}
}

BlendMode parseBlendMode(std::string_view raw) {
    const std::string value = trim(raw);
    if (value == "add" || value == "additive" || value == "lighter") return BlendMode::Additive;
    if (value == "multiply") return BlendMode::Multiply;
    if (value == "screen") return BlendMode::Screen;
    if (value == "subtract" || value == "subtractive") return BlendMode::Subtract;
    return BlendMode::Normal;
}

ShadingMode parseShadingMode(std::string_view raw) {
    const std::string value = trim(raw);
    if (value == "gouraud" || value == "smooth" || value == "auto" ||
        value == "auto-smooth") return ShadingMode::Gouraud;
    if (value == "unlit" || value == "emissive") return ShadingMode::Unlit;
    return ShadingMode::Flat;
}

bool Selector::matches(const Node& node) const {
    if (state) {
        UiState current = node.disabled ? UiState::Disabled : node.state;
        if (current != *state) return false;
    }
    if (kind == Kind::Id) return node.id == name;
    if (kind == Kind::Class) return node.hasClass(name);
    return node.element == name;
}

int Selector::specificity() const { return kind == Kind::Id ? 100 : kind == Kind::Class ? 10 : 1; }

std::optional<Paint> parsePaint(std::string_view raw) {
    std::string value = trim(raw); if (auto c = Color::parse(value)) return *c;
    if (value.starts_with("rgb(") || value.starts_with("rgba(")) return color(value);
    if (value == "red" || value == "blue" || value == "white" || value == "black" || value == "transparent") return namedColor(value);
    auto pieces = splitTop(inner(value));
    if (value.starts_with("linear-gradient")) {
        LinearGradient gradient; std::size_t start = 0;
        if (!pieces.empty() && (pieces[0].find("deg") != std::string::npos || pieces[0].starts_with("to "))) {
            if (pieces[0] == "to bottom") gradient.angle = 180; else if (pieces[0] == "to right") gradient.angle = 90; else gradient.angle = dimension(pieces[0]); ++start;
        }
        gradient.stops = stops(pieces, start); if (gradient.stops.empty()) return std::nullopt; return gradient;
    }
    if (value.starts_with("radial-gradient")) { RadialGradient gradient{stops(pieces, 0)}; if (gradient.stops.empty()) return std::nullopt; return gradient; }
    const std::pair<std::string_view, Pattern::Kind> patterns[]{
        {"dotted-texture", Pattern::Kind::Dots}, {"stripes", Pattern::Kind::Stripes}, {"checker", Pattern::Kind::Checker}, {"grid", Pattern::Kind::Grid}};
    for (auto [name, kind] : patterns) if (value.starts_with(name)) {
        Pattern pattern{kind, pieces.empty() ? Color{} : color(pieces[0]), pieces.size() > 1 ? dimension(pieces[1], 3) : 3, pieces.size() > 2 ? dimension(pieces[2], 18) : 18};
        return pattern;
    }
    return std::nullopt;
}

void StyleSheet::parse(std::string_view source) {
    rules_.clear(); std::size_t pos = 0, sequence = 0;
    while (pos < source.size()) {
        while (pos < source.size() && std::isspace(static_cast<unsigned char>(source[pos]))) ++pos;
        if (source.substr(pos, 2) == "/*") { auto end = source.find("*/", pos + 2); pos = end == std::string_view::npos ? source.size() : end + 2; continue; }
        auto brace = source.find('{', pos); if (brace == std::string_view::npos) break;
        auto end = source.find('}', brace + 1); if (end == std::string_view::npos) throw std::runtime_error("Unclosed CafeStyle rule");
        StyleRule rule; rule.selector = parseSelector(trim(source.substr(pos, brace - pos))); rule.sequence = sequence++;
        auto body = source.substr(brace + 1, end - brace - 1); std::size_t d = 0;
        while (d < body.size()) {
            auto colon = body.find(':', d); if (colon == std::string_view::npos) break;
            int depth = 0; std::size_t semi = colon + 1;
            for (; semi < body.size(); ++semi) { if (body[semi] == '(') ++depth; else if (body[semi] == ')') --depth; else if (body[semi] == ';' && depth == 0) break; }
            auto key = trim(body.substr(d, colon - d)); auto val = trim(body.substr(colon + 1, semi - colon - 1)); if (!key.empty()) rule.declarations[key] = val; d = semi + 1;
        }
        rules_.push_back(std::move(rule)); pos = end + 1;
    }
}

void StyleSheet::load(const std::filesystem::path& path) {
    parse(readTextFile(path));
}

ComputedStyle StyleSheet::compute(const Node& node) const {
    ComputedStyle out; std::vector<const StyleRule*> matched;
    for (auto& rule : rules_) if (rule.selector.matches(node)) matched.push_back(&rule);
    std::stable_sort(matched.begin(), matched.end(), [](auto a, auto b) {
        return a->selector.specificity() == b->selector.specificity() ? a->sequence < b->sequence : a->selector.specificity() < b->selector.specificity();
    });
    for (auto* rule : matched) for (auto& [key, value] : rule->declarations) applyDeclaration(out, key, value);
    return out;
}

} // namespace cafe
