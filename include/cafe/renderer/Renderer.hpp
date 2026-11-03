#pragma once

#include "cafe/assets/AssetManager.hpp"
#include "cafe/core/Node.hpp"
#include "cafe/cstyle/Style.hpp"

#include <filesystem>
#include <cstddef>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cafe {

class FontFace;

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual void beginDisplay(Display display, Color clear) = 0;
    virtual bool beginDisplayFrame(Display display, Color clear, std::size_t) {
        beginDisplay(display, clear);
        return true;
    }
    virtual void setMirrorDisplays(bool) {}
    virtual void setExpandedViewport(bool) {}
    // Returns true after the backend detects and applies new output dimensions.
    virtual bool refreshOutputGeometry() { return false; }
    [[nodiscard]] virtual Rect visibleBounds(Display) const = 0;
    virtual void drawSprite(const Node&, const ComputedStyle&, const Asset* texture) = 0;
    virtual void drawText(const Node&, const ComputedStyle&, const Asset* font = nullptr) = 0;
    virtual void drawRectangle(const Node&, const ComputedStyle&) = 0;
    virtual void drawPanel(const Node&, const ComputedStyle&) = 0;
    virtual void drawParticles(const Node&, const ComputedStyle&, const Asset* texture) = 0;
    virtual void drawSpriteBatch(const Node&, const ComputedStyle&, const Asset* texture) = 0;
    virtual void drawShape(const Node&, const ComputedStyle&) = 0;
    virtual void drawMesh3D(const Node&, const ComputedStyle&, const Asset* mesh,
                            const Node* camera) = 0;
    virtual void endDisplay(Display) {}
    virtual void present() = 0;
};

class SoftwareRenderer : public Renderer {
public:
    SoftwareRenderer(int logicalWidth, int logicalHeight,
                     std::filesystem::path output = {}, double renderScale = 1.0);
    ~SoftwareRenderer() override;
    void beginDisplay(Display display, Color clear) override;
    void drawSprite(const Node&, const ComputedStyle&, const Asset* texture) override;
    void drawText(const Node&, const ComputedStyle&, const Asset* font = nullptr) override;
    void drawRectangle(const Node&, const ComputedStyle&) override;
    void drawPanel(const Node&, const ComputedStyle&) override;
    void drawParticles(const Node&, const ComputedStyle&, const Asset* texture) override;
    void drawSpriteBatch(const Node&, const ComputedStyle&, const Asset* texture) override;
    void drawShape(const Node&, const ComputedStyle&) override;
    void drawMesh3D(const Node&, const ComputedStyle&, const Asset* mesh,
                    const Node* camera) override;
    void present() override;
    [[nodiscard]] const std::filesystem::path& outputPath() const { return output_; }
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    [[nodiscard]] int logicalWidth() const { return logicalWidth_; }
    [[nodiscard]] int logicalHeight() const { return logicalHeight_; }
    [[nodiscard]] Rect visibleBounds(Display) const override {
        return {0, 0, static_cast<double>(logicalWidth_),
                static_cast<double>(logicalHeight_)};
    }
    [[nodiscard]] double renderScale() const { return renderScale_; }
    [[nodiscard]] const std::vector<Color>& pixels(Display display) const {
        return display == Display::GamePad ? gamePadPixels_ : tvPixels_;
    }
protected:
    int width_, height_;
    int logicalWidth_, logicalHeight_;
    double renderScale_{1.0};
    std::filesystem::path output_;
    std::vector<Color> tvPixels_;
    std::vector<Color> gamePadPixels_;
    std::vector<Color>* currentPixels_{&tvPixels_};
    Display currentDisplay_{Display::Tv};
private:
    std::vector<double> depthBuffer_;
    BlendMode currentBlendMode_{BlendMode::Normal};
    std::unordered_map<std::string, std::unique_ptr<FontFace>> fonts_;
    struct BatchRasterCache {
        std::size_t signature{0};
        std::vector<Color> pixels;
        std::vector<std::uint32_t> activePixels;
    };
    std::unordered_map<std::string, BatchRasterCache> batchRasterCache_;
    [[nodiscard]] Node scaledNode(const Node&) const;
    [[nodiscard]] ComputedStyle scaledStyle(const ComputedStyle&) const;
    void drawRectanglePixels(const Node&, const ComputedStyle&);
    void pixel(int x, int y, Color color, double opacity = 1.0);
    void blendPixel(Color& destination, Color source, double opacity);
    void fillSpan(int y, int x0, int x1, Color color, double opacity);
    void paintRect(const Rect&, const Paint&, double opacity, double radius = 0);
    void paintGrid(const Rect&, const Pattern&, double opacity, double radius);
    void paintDots(const Rect&, const Pattern&, double opacity, double radius);
    void shadow(const Rect&, const Shadow&, double radius, const Sawtooth* sawtooth = nullptr);
    void paintSawtooth(const Rect&, const Sawtooth&, const Paint&, double opacity,
                       double radius, double inset = 0);
    void shadeSawtoothInset(const Rect&, const Sawtooth&, const Shadow&, double radius,
                            double borderWidth);
    void paintStyledShape(const Rect&, const ComputedStyle&, double opacity);
    void writePpm(const std::filesystem::path& path) const;
};

} // namespace cafe
