#pragma once

#include "cafe/core/Node.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace cafe {

struct ColorStop { double position; Color color; };
struct LinearGradient { double angle{180}; std::vector<ColorStop> stops; };
struct RadialGradient { std::vector<ColorStop> stops; };
struct Pattern { enum class Kind { Dots, Stripes, Checker, Grid } kind; Color color; double size{3}; double spacing{18}; };
using Paint = std::variant<Color, LinearGradient, RadialGradient, Pattern>;

struct Shadow { double x{0}, y{0}, blur{0}; Color color{}; bool inset{false}; };
struct Sawtooth { std::string side{"bottom"}; double tooth{14}; double depth{8}; };
enum class BlendMode { Normal, Additive, Multiply, Screen, Subtract };
enum class ShadingMode { Flat, Gouraud, Unlit };

struct ComputedStyle {
    std::optional<Paint> fill;
    std::vector<Paint> backgrounds;
    double borderWidth{0};
    Color borderColor{};
    double borderRadius{0};
    std::vector<Shadow> shadows;
    std::optional<Sawtooth> sawtooth;
    Color textColor{255, 255, 255, 255};
    double fontSize{20};
    std::string fontSource;
    BlendMode blendMode{BlendMode::Normal};
    ShadingMode shadingMode{ShadingMode::Flat};
};

struct Selector {
    enum class Kind { Element, Class, Id } kind{Kind::Element};
    std::string name;
    std::optional<UiState> state;
    [[nodiscard]] bool matches(const Node& node) const;
    [[nodiscard]] int specificity() const;
};

struct StyleRule {
    Selector selector;
    std::unordered_map<std::string, std::string> declarations;
    std::size_t sequence{0};
};

class StyleSheet {
public:
    void parse(std::string_view source);
    void load(const std::filesystem::path& path);
    [[nodiscard]] ComputedStyle compute(const Node& node) const;
    [[nodiscard]] const std::vector<StyleRule>& rules() const { return rules_; }
private:
    std::vector<StyleRule> rules_;
};

std::optional<Paint> parsePaint(std::string_view value);
BlendMode parseBlendMode(std::string_view value);
ShadingMode parseShadingMode(std::string_view value);

} // namespace cafe

