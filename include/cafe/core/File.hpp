#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cafe {

[[nodiscard]] bool fileReadable(const std::filesystem::path& path);
[[nodiscard]] std::string readTextFile(const std::filesystem::path& path);
[[nodiscard]] std::vector<std::uint8_t> readBinaryFile(const std::filesystem::path& path);

} // namespace cafe
