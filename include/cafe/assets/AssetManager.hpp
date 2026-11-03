#pragma once

#include "cafe/core/Types.hpp"

#include <filesystem>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cafe {

enum class AssetType { Image, Font, Audio, Mesh };
struct MeshVertex { Vec3 position{}; Vec3 normal{}; Vec2 uv{}; };
struct Asset {
    AssetType type;
    std::filesystem::path path;
    bool available{false};
    int width{0}, height{0};
    std::vector<std::uint8_t> bytes;
    std::vector<Color> pixels;
    int sampleRate{0};
    int channels{0};
    std::vector<std::int16_t> samples;
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::uint64_t revision{0}; // Changes when a bounded procedural slot is reused.
};

// Coordinates and outward normals are normalized to the image's 0..1 bounds.
struct MaskEdgePoint { double x{0},y{0},nx{0},ny{0}; };

class AssetManager {
public:
    void setRoot(std::filesystem::path root) { root_ = std::move(root); }
    const Asset& loadImage(std::string_view path);
    const Asset& loadFont(std::string_view path);
    const Asset& loadAudio(std::string_view path);
    const Asset& loadMesh(std::string_view path);
    std::vector<MaskEdgePoint> edgePoints(std::string_view path,int spacing=16,
                                          double threshold=0.5);
    [[nodiscard]] const Asset* get(std::string_view path) const;
private:
    const Asset& load(std::string_view path, AssetType type);
    const Asset& loadProcedural(std::string_view source);
    struct ProceduralSlot {
        Asset asset{};
        std::string source;
        std::uint64_t lastUsed{0};
    };
    static constexpr std::size_t maxProceduralSlots = 24;
    std::filesystem::path root_;
    std::unordered_map<std::string, Asset> assets_;
    std::vector<std::unique_ptr<ProceduralSlot>> proceduralSlots_;
    std::unordered_map<std::string,std::size_t> proceduralIndex_;
    std::uint64_t proceduralClock_{0};
    struct EdgeCacheEntry {
        std::uint64_t revision{0};
        int spacing{0};
        double threshold{-1};
        std::vector<MaskEdgePoint> points;
    };
    std::unordered_map<const Asset*,EdgeCacheEntry> edgeCache_;
};

} // namespace cafe
