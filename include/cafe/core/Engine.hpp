#pragma once

#include "cafe/assets/AssetManager.hpp"
#include "cafe/audio/AudioSystem.hpp"
#include "cafe/cafescript/Interpreter.hpp"
#include "cafe/cstyle/Style.hpp"
#include "cafe/input/Input.hpp"
#include "cafe/renderer/Renderer.hpp"
#include "cafe/uml/UmlParser.hpp"

#include <filesystem>
#include <functional>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace cafe {

class Engine {
public:
    explicit Engine(std::unique_ptr<Renderer> renderer);
    void setStorageRoot(std::filesystem::path root) {
        storageRoot_ = std::move(root);
        storageRootConfigured_ = true;
    }
    void loadProject(const std::filesystem::path& umlPath);
    bool refreshDisplayLayout();
    void update(double deltaSeconds);
#ifdef CAFE_RENDER_PROFILE
    struct UpdateStats { double hoverMs{0}, navigationMs{0}, scriptMs{0}, particlesMs{0}; };
    [[nodiscard]] UpdateStats lastUpdateStats() const { return lastUpdateStats_; }
#endif
    void render();
    // Optional first-frame diagnostics; leave unset during normal gameplay.
    void setStartupTrace(std::function<void(const char*)> trace) {
        startupTrace_ = std::move(trace);
    }
    void click(double x, double y);
    void click(double x, double y, Display display);
    [[nodiscard]] SceneGraph& sceneGraph() { return project_->graph; }
    [[nodiscard]] const SceneGraph& sceneGraph() const { return project_->graph; }
    [[nodiscard]] const StyleSheet& styles() const { return styles_; }
    [[nodiscard]] InputSystem& input() { return input_; }
    [[nodiscard]] const InputSystem& input() const { return input_; }
    [[nodiscard]] Renderer& renderer() { return *renderer_; }
    [[nodiscard]] const Renderer& renderer() const { return *renderer_; }
private:
#ifdef CAFE_RENDER_PROFILE
    UpdateStats lastUpdateStats_{};
#endif
    std::unique_ptr<Renderer> renderer_;
    std::function<void(const char*)> startupTrace_{};
    std::optional<UmlResult> project_;
    StyleSheet styles_;
    InputSystem input_;
    AssetManager assets_;
    AudioSystem audio_;
    std::unique_ptr<script::Interpreter> script_;
    std::filesystem::path storageRoot_;
    bool storageRootConfigured_{false};
    double elapsed_{0};
    std::string focusedId_;
    struct CachedNodeStyles {
        std::string id;
        std::string element;
        std::array<std::optional<ComputedStyle>, 5> states;
    };
    std::unordered_map<const Node*, CachedNodeStyles> styleCache_;

    void renderDisplay(Display display);
    [[nodiscard]] std::size_t displaySignature(Display display,
                                                const std::vector<Node*>& nodes) const;
    const ComputedStyle& computedStyle(const Node& node);
    void refreshHover(Display display, double x, double y, bool pressed);
    void applyPointerHover();
    void navigateUi();
    void focusDefault();
    void loadNodeAssets(Node* node);
    void updateParticles(Node* node, double deltaSeconds);
    bool clickOn(Display display, double x, double y);
    [[nodiscard]] Rect screenBounds(const Node& node, Display display) const;
    std::vector<Node*> focusableButtons() const;
    Node* focusedButton() const;
    [[nodiscard]] std::optional<std::filesystem::path> storagePath(std::string_view key) const;
};

} // namespace cafe
