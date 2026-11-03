#pragma once

#include "cafe/renderer/Renderer.hpp"

#ifdef CAFE_PLATFORM_WIIU

#include <gx2/sampler.h>
#include <gx2/texture.h>
#include <gx2r/buffer.h>
#include <whb/gfx.h>
#include <chrono>
#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <tuple>
#include <memory>
#include <unordered_map>

namespace cafe {

// Wii U backend. Textured 2D quads and retained sprite batches use GX2.
// Unsupported 2D operations are rasterized into ordered, cropped layers.
class WiiURenderer final : public SoftwareRenderer {
public:
    struct FrameStats {
        double cpuFrameMs{0};
        double frameMs{0};
        double fps{0};
        double rasterMs{0};
        double geometryMs{0};
        double uploadMs{0};
        double presentMs{0};
        std::size_t drawCalls{0};
        std::size_t textureUploads{0};
        std::size_t uploadedBytes{0};
        std::size_t sprites{0};
        std::size_t particles{0};
    };
    WiiURenderer(int logicalWidth = 640, int logicalHeight = 480);
    ~WiiURenderer() override;

    WiiURenderer(const WiiURenderer&) = delete;
    WiiURenderer& operator=(const WiiURenderer&) = delete;

    void present() override;
    void beginDisplay(Display display, Color clear) override;
    bool beginDisplayFrame(Display display, Color clear, std::size_t signature) override;
    void setMirrorDisplays(bool mirror) override;
    void setExpandedViewport(bool expanded) override;
    bool refreshOutputGeometry() override;
    [[nodiscard]] Rect visibleBounds(Display display) const override;
    void drawSprite(const Node&, const ComputedStyle&, const Asset*) override;
    void drawSpriteBatch(const Node&, const ComputedStyle&, const Asset*) override;
    void drawText(const Node&, const ComputedStyle&, const Asset*) override;
    void drawRectangle(const Node&, const ComputedStyle&) override;
    void drawPanel(const Node&, const ComputedStyle&) override;
    void drawShape(const Node&, const ComputedStyle&) override;
    void endDisplay(Display display) override;
    void drawParticles(const Node&, const ComputedStyle&, const Asset* texture) override;
    void drawMesh3D(const Node&, const ComputedStyle&, const Asset* mesh,
                    const Node* camera) override;
    [[nodiscard]] const FrameStats& lastFrameStats() const { return lastStats_; }
    // Only used for first-frame hardware diagnostics; cleared after startup.
    void setStartupTrace(std::function<void(const char*)> trace);
    void setGpu2DEnabled(bool enabled);
    [[nodiscard]] int outputWidth(Display display) const;
    [[nodiscard]] int outputHeight(Display display) const;
    [[nodiscard]] const Rect& viewport(Display display) const;
    [[nodiscard]] std::optional<Vec2> outputToLogical(Display display,
                                                       double x, double y) const;

private:
    struct UploadSurface {
        GX2Texture texture{};
        int width{0};
        int height{0};
        std::vector<std::uint16_t> mapX;
        std::vector<std::uint16_t> mapY;
    };
    struct Draw2DCommand {
        std::uint32_t first{0};
        std::uint32_t count{0};
        const Asset* asset{nullptr};
        std::size_t layer{0};
        bool softwareLayer{false};
        BlendMode blend{BlendMode::Normal};
    };
    struct SoftwareLayer {
        int width{0}, height{0};
        std::vector<Color> pixels;
        UploadSurface* upload{nullptr}; // Owned by the per-display texture pool.
    };
    struct Frame2D {
        std::vector<float> positions, textureCoordinates, colors;
        std::vector<Draw2DCommand> commands;
        std::vector<SoftwareLayer> layers;
        std::vector<std::unique_ptr<UploadSurface>> uploadPool;
        Color clear{0,0,0,255};
        std::size_t revision{0};
        std::size_t spriteCount{0};
        bool softwarePending{false};
        bool softwareInitialized{false};
        int scanLeft{0},scanTop{0},scanRight{0},scanBottom{0};
    };
    struct FontAtlas {
        struct Glyph { int x{0},y{0},width{0},height{0},xoff{0},yoff{0}; float advance{0}; };
        Asset asset{};
        std::unordered_map<int,Glyph> glyphs;
        double scale{1.0};
        std::unique_ptr<FontFace> face;
    };
    // A panel's visual parameters form a small exact cache key. Building it
    // in fixed storage avoids heap/string mutation during the first Wii U
    // frame. Rare styles exceeding this capacity use the software fallback.
    struct ShapeKey {
        std::array<std::uint8_t,512> data{};
        std::size_t size{0};
        bool operator==(const ShapeKey& other) const noexcept {
            return size==other.size &&
                std::equal(data.begin(),data.begin()+size,other.data.begin());
        }
    };
    struct ShapeKeyHash {
        std::size_t operator()(const ShapeKey& key) const noexcept {
            std::size_t hash=static_cast<std::size_t>(2166136261u);
            for (std::size_t i=0;i<key.size;++i)
                hash=(hash^key.data[i])*static_cast<std::size_t>(16777619u);
            return hash;
        }
    };

    UploadSurface tvUpload_{};
    UploadSurface gamePadUpload_{};
    WHBGfxShaderGroup shader_{};
    GX2RBuffer tvPositionBuffer_{};
    GX2RBuffer gamePadPositionBuffer_{};
    GX2RBuffer textureCoordinateBuffer_{};
    GX2RBuffer colorBuffer_{};
    GX2Sampler sampler_{};
    struct MeshBatch {
        struct Range {
            std::uint32_t first{0};
            std::uint32_t count{0};
            BlendMode blendMode{BlendMode::Normal};
            bool depthWrite{true};
            bool depthTest{true};
        };
        std::vector<float> positions;
        std::vector<float> textureCoordinates;
        std::vector<float> colors;
        std::vector<Range> ranges;
    };
    struct MeshBuffers {
        GX2RBuffer positions{};
        GX2RBuffer textureCoordinates{};
        GX2RBuffer colors{};
    };
    MeshBatch tvMeshes_{};
    MeshBatch gamePadMeshes_{};
    MeshBuffers tvMeshBuffers_{};
    MeshBuffers gamePadMeshBuffers_{};
    Frame2D tv2D_{};
    Frame2D gamePad2D_{};
    std::array<MeshBuffers,3> tv2DBuffers_{};
    std::array<MeshBuffers,3> gamePad2DBuffers_{};
    std::array<std::size_t,3> tv2DBufferRevision_{};
    std::array<std::size_t,3> gamePad2DBufferRevision_{};
    std::size_t presentCounter_{0};
    std::size_t next2DRevision_{1};
    std::unordered_map<const Asset*, std::unique_ptr<UploadSurface>> imageTextures_;
    std::unordered_map<const Asset*,std::uint64_t> imageTextureRevisions_;
    struct FilteredAsset {
        Asset image{};
        std::uint64_t sourceRevision{0};
    };
    std::map<std::tuple<const Asset*,int,int>,std::unique_ptr<FilteredAsset>> filteredAssets_;
    std::size_t filteredAssetBytes_{0};
    std::unordered_map<ShapeKey,std::unique_ptr<Asset>,ShapeKeyHash> shapeTextures_;
    std::size_t shapeTextureBytes_{0};
    std::map<std::pair<const Asset*,int>,std::unique_ptr<FontAtlas>> fontAtlases_;
    std::size_t imageTextureBytes_{0};
    UploadSurface whiteTexture_{};
    Rect tvViewport_{};
    Rect gamePadViewport_{};
    Rect tvVisibleBounds_{};
    Rect gamePadVisibleBounds_{};
    int tvOutputWidth_{0};
    int tvOutputHeight_{0};
    int gamePadOutputWidth_{0};
    int gamePadOutputHeight_{0};
    // Shared UI textures are rasterized for the densest fitted display.
    // Both screens keep the same logical coordinates and hit targets.
    double uiTextureScale_{1.0};
    bool graphicsReady_{false};
    bool shaderReady_{false};
    bool tvFrameValid_{false};
    bool gamePadFrameValid_{false};
    bool tvPixelsDirty_{true};
    bool gamePadPixelsDirty_{true};
    bool mirrorDisplays_{false};
    bool expandedViewport_{false};
    std::size_t tvFrameSignature_{0};
    std::size_t gamePadFrameSignature_{0};
    bool gpu2DEnabled_{true};
    std::function<void(const char*)> startupTrace_{};
    FrameStats frameStats_{};
    FrameStats lastStats_{};
    std::chrono::steady_clock::time_point frameStart_{};
    std::chrono::steady_clock::time_point previousPresent_{};

    void createPresentationShader();
    void createPositionBuffer(GX2RBuffer& buffer, const Rect& viewport,
                              int outputWidth, int outputHeight);
    void updateViewports();
    void clearUiTextureCaches();
    void destroyPresentationShader();
    void createUploadSurface(UploadSurface& upload, int width, int height, bool flipY);
    static void destroyUploadSurface(UploadSurface& upload);
    bool upload(UploadSurface& upload, const std::vector<Color>& source);
    void drawUpload(const UploadSurface& upload, GX2RBuffer& positionBuffer);
    static void destroyMeshBuffers(MeshBuffers& buffers);
    void drawMeshes(const MeshBatch& batch, MeshBuffers& buffers,
                    Display display);
    Frame2D& current2D();
    void ensureSoftwareTarget();
    void reset2DFrame(Frame2D&, Color clear);
    void markSoftwareBounds(Frame2D&,const Node&,const ComputedStyle&,bool text=false);
    void flushSoftwareLayer(Frame2D&);
    void queueQuad(Frame2D&, Rect rect, double rotation, Color tint,
                   double opacity, BlendMode blend, const Asset* asset,
                   std::size_t layer, bool softwareLayer,
                   double uMax = 1.0, double vMax = 1.0,
                   double uMin = 0.0, double vMin = 0.0);
    const UploadSurface* imageTexture(const Asset* asset);
    const Asset* filteredTexture(const Asset* source,double hue,double saturation);
    FontAtlas* fontAtlas(const Asset* font,int size);
    const Asset* shapeTexture(const Node&,const ComputedStyle&,bool shape);
    bool uploadPixels(UploadSurface&, const Color*, int width, int height);
    bool prepare2D(Frame2D&, MeshBuffers&, std::size_t& bufferRevision,
                   Display display);
    void draw2D(Frame2D&, MeshBuffers&, Display display);
    void setBlendMode(BlendMode mode);
};

} // namespace cafe

#endif
