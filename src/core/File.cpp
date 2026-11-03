#include "cafe/core/File.hpp"

#include <cstdio>
#include <stdexcept>

namespace cafe {
namespace {

FILE* openFile(const std::filesystem::path& path, const char* mode) {
    return std::fopen(path.string().c_str(), mode);
}

std::vector<std::uint8_t> readAll(const std::filesystem::path& path) {
    FILE* file = openFile(path, "rb");
    if (!file) throw std::runtime_error("Cannot open file: " + path.string());
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        throw std::runtime_error("Cannot seek file: " + path.string());
    }
    const long size = std::ftell(file);
    if (size < 0) {
        std::fclose(file);
        throw std::runtime_error("Cannot size file: " + path.string());
    }
    std::rewind(file);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0 && std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) {
        std::fclose(file);
        throw std::runtime_error("Cannot read file: " + path.string());
    }
    std::fclose(file);
    return bytes;
}

} // namespace

bool fileReadable(const std::filesystem::path& path) {
    FILE* file = openFile(path, "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
}

std::string readTextFile(const std::filesystem::path& path) {
    const auto bytes = readAll(path);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::vector<std::uint8_t> readBinaryFile(const std::filesystem::path& path) {
    return readAll(path);
}

} // namespace cafe
