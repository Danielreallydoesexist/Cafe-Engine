#pragma once

#include "cafe/core/Node.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace cafe {

struct ProjectLinks {
    std::filesystem::path style;
    std::filesystem::path script;
    std::string inlineStyle;
    std::string inlineScript;
};

struct UmlResult {
    SceneGraph graph;
    ProjectLinks links;
    std::vector<std::string> warnings;
};

class UmlParser {
public:
    UmlResult parseFile(const std::filesystem::path& path) const;
    UmlResult parse(std::string_view source, const std::filesystem::path& base = {}) const;
};

} // namespace cafe

