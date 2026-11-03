#include "cafe/core/Engine.hpp"
#include "cafe/core/File.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <functional>
#include <fstream>
#ifdef CAFE_RENDER_PROFILE
#include <chrono>
#endif

namespace cafe {

Engine::Engine(std::unique_ptr<Renderer> renderer) : renderer_(std::move(renderer)) {}

bool Engine::refreshDisplayLayout() {
    if (!renderer_->refreshOutputGeometry()) return false;
    if (script_)
        script_->setScreenBounds(renderer_->visibleBounds(Display::Tv),
                                 renderer_->visibleBounds(Display::GamePad));
    return true;
}

std::optional<std::filesystem::path> Engine::storagePath(std::string_view key) const {
    if (storageRoot_.empty() || key.empty() || key.size() > 96) return std::nullopt;
    if (key == "." || key == ".." || key.find("..") != std::string_view::npos) return std::nullopt;
    for (const char c : key) {
        const bool safe = std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
                          c == '_' || c == '.';
        if (!safe) return std::nullopt;
    }
    return storageRoot_ / std::string(key);
}

void Engine::loadNodeAssets(Node* node) {
    if (!node) return;
    if (!node->source.empty()) {
        if (node->type == NodeType::Audio) assets_.loadAudio(node->source);
        else if (node->type == NodeType::Mesh3D) assets_.loadMesh(node->source);
        else assets_.loadImage(node->source);
    }
    for (auto& child : node->children) loadNodeAssets(child.get());
}

void Engine::loadProject(const std::filesystem::path& umlPath) {
    styleCache_.clear();
    styles_ = StyleSheet{};
    project_ = UmlParser{}.parseFile(umlPath);
    for (const auto& child : project_->graph.root().children) {
        if (child->type != NodeType::Main) continue;
        const auto viewport = child->attributes.find("viewport");
        renderer_->setExpandedViewport(viewport != child->attributes.end() &&
                                        viewport->second == "expand");
        break;
    }
    assets_.setRoot(umlPath.parent_path());
    if (!storageRootConfigured_) storageRoot_ = umlPath.parent_path() / ".cafe-save";
    std::string styleSource;
    if (!project_->links.style.empty()) styleSource = readTextFile(project_->links.style);
    if (!project_->links.inlineStyle.empty()) {
        if (!styleSource.empty()) styleSource.push_back('\n');
        styleSource += project_->links.inlineStyle;
    }
    if (!styleSource.empty()) styles_.parse(styleSource);
    // Images and audio resolve on first use. Eagerly decoding every scene,
    // including boss audio and hidden cave assets, kept the Wii U on its boot
    // logo for a long time and raised peak startup memory. Spawned nodes still
    // use loadNodeAssets through the callback below.
    script_ = std::make_unique<script::Interpreter>(project_->graph, input_);
    script_->setScreenBounds(renderer_->visibleBounds(Display::Tv),
                             renderer_->visibleBounds(Display::GamePad));
    script_->setSpawnCallback([this](Node* node) { loadNodeAssets(node); });
    script_->setAudioPlayCallback([this](std::string_view source, bool loop, double volume) {
        return audio_.play(assets_.loadAudio(source), loop, volume);
    });
    script_->setAudioStopCallback([this](int voice) {
        if (voice <= 0) audio_.stopAll(); else audio_.stop(voice);
    });
    script_->setAudioControlCallback([this](int voice, std::string_view action, double value) {
        if (voice <= 0) return;
        if (action == "pause") audio_.pause(voice);
        else if (action == "resume") audio_.resume(voice);
        else if (action == "volume") audio_.setVolume(voice, value);
        else if (action == "pitch") audio_.setPitch(voice, value);
    });
    script_->setAudioStateCallback([this](int voice) { return audio_.isPlaying(voice); });
    script_->setStorageCallbacks(
        [this](std::string_view key) {
            const auto path = storagePath(key);
            if (!path) return std::string{};
            std::ifstream input(*path, std::ios::binary);
            if (!input) return std::string{};
            return std::string(std::istreambuf_iterator<char>(input), {});
        },
        [this](std::string_view key, std::string_view value) {
            const auto path = storagePath(key);
            if (!path) return false;
            std::error_code error;
            std::filesystem::create_directories(path->parent_path(), error);
            if (error) return false;
            std::ofstream output(*path, std::ios::binary | std::ios::trunc);
            if (!output) return false;
            output.write(value.data(), static_cast<std::streamsize>(value.size()));
            return output.good();
        },
        [this](std::string_view key) {
            const auto path = storagePath(key);
            std::error_code error;
            return path && std::filesystem::is_regular_file(*path, error) && !error;
        },
        [this](std::string_view key) {
            const auto path = storagePath(key);
            std::error_code error;
            return path && std::filesystem::remove(*path, error) && !error;
        });
    script_->setAlphaHitCallback([this](std::string_view source, double x,
                                        double y, double threshold) {
        const Asset& image = assets_.loadImage(source);
        const int px = static_cast<int>(std::floor(x));
        const int py = static_cast<int>(std::floor(y));
        if (!image.available || px < 0 || py < 0 || px >= image.width || py >= image.height)
            return false;
        const auto alpha = image.pixels[static_cast<std::size_t>(py * image.width + px)].a;
        return alpha / 255.0 >= std::clamp(threshold, 0.0, 1.0);
    });
    script_->setMaskEdgesCallback([this](std::string_view source,int spacing,
                                         double threshold) {
        return assets_.edgePoints(source,spacing,threshold);
    });
    std::string scriptSource;
    if (!project_->links.script.empty()) scriptSource = readTextFile(project_->links.script);
    if (!project_->links.inlineScript.empty()) {
        if (!scriptSource.empty()) scriptSource.push_back('\n');
        scriptSource += project_->links.inlineScript;
    }
    if (!scriptSource.empty()) script_->load(scriptSource);
    script_->runLoad();
    std::vector<Node*> pending{&project_->graph.root()};
    while (!pending.empty()) {
        Node* node = pending.back(); pending.pop_back();
        if (node->type == NodeType::Audio && !node->source.empty() &&
            boolean(node->attributes.contains("autoplay") ? node->attributes.at("autoplay") : "", false)) {
            audio_.play(assets_.loadAudio(node->source),
                        boolean(node->attributes.contains("loop") ? node->attributes.at("loop") : "", false),
                        number(node->attributes.contains("volume") ? node->attributes.at("volume") : "", 1.0));
        }
        for (auto& child : node->children) pending.push_back(child.get());
    }
    focusDefault();
}

const ComputedStyle& Engine::computedStyle(const Node& node) {
    const UiState effectiveState = node.disabled ? UiState::Disabled : node.state;
    const auto stateIndex = static_cast<std::size_t>(effectiveState);
    auto& entry = styleCache_[&node];
    // Spawned nodes may reuse an address after an earlier node was destroyed.
    if (entry.id != node.id || entry.element != node.element) {
        entry = {};
        entry.id = node.id;
        entry.element = node.element;
    }
    auto& cached = entry.states[stateIndex];
    if (!cached) cached = styles_.compute(node);
    return *cached;
}

std::size_t Engine::displaySignature(Display display, const std::vector<Node*>& nodes) const {
    std::size_t seed = display == Display::GamePad ? 0x9e3779b9u : 0x85ebca6bu;
    auto combine = [&](std::size_t value) {
        seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    };
    auto numberHash = [&](double value) { combine(std::hash<double>{}(value)); };
    for (const Node* node : nodes) {
        if (node->type == NodeType::Camera3D || node->type == NodeType::Mesh3D ||
            node->type == NodeType::Cube3D || node->type == NodeType::ParticleEmitter ||
            node->type == NodeType::Audio) continue;
        combine(static_cast<std::size_t>(node->type));
        combine(std::hash<std::string>{}(node->id));
        combine(node->visible); numberHash(node->opacity);
        numberHash(node->hue);numberHash(node->saturation);
        numberHash(node->transform.position.x); numberHash(node->transform.position.y);
        numberHash(node->transform.rotation); numberHash(node->transform.scale.x);
        numberHash(node->transform.scale.y); numberHash(node->width); numberHash(node->height);
        combine(static_cast<std::size_t>(node->state)); combine(node->disabled);
        combine(std::hash<std::string>{}(node->text));
        combine(std::hash<std::string>{}(node->source));
        if (node->type == NodeType::SpriteBatch) combine(node->batch.revision);
    }
    if (display == Display::Tv) {
        combine(input_.wiiRemote.pointerValid);
        if (input_.wiiRemote.pointerValid) {
            numberHash(input_.wiiRemote.x);
            numberHash(input_.wiiRemote.y);
        }
    }
    return seed;
}

void Engine::renderDisplay(Display display) {
    const auto renderList = project_->graph.renderList(display);
    const bool draw2D = renderer_->beginDisplayFrame(
        display, {18, 25, 38, 255}, displaySignature(display, renderList));
    const auto traceNode=[this,display](const Node& node,bool done) {
        if (!startupTrace_) return;
        std::string stage=display==Display::Tv?"tv-":"pad-";
        stage+=done?"done:":"node:";
        stage+=!node.id.empty()?node.id:(!node.source.empty()?node.source:node.element);
        if (stage.size()>96) stage.resize(96);
        startupTrace_(stage.c_str());
    };
    const Node* camera = nullptr;
    for (Node* candidate : renderList) {
        if (candidate->type == NodeType::Camera3D && candidate->visible) { camera = candidate; break; }
    }
    // CafeScript can replace src after project loading. Resolving on render
    // also refreshes the bounded procedural cave cache for visible chunks.
    const auto imageFor = [this](const Node& image) -> const Asset* {
        return image.source.empty() ? nullptr : &assets_.loadImage(image.source);
    };
    for (Node* node : renderList) {
        if (!node->visible || node->opacity <= 0) continue;
        // Cave masks are expensive to generate and upload. Their scene nodes
        // cover nearby chunks, including chunks wholly outside this display.
        // Skip those before resolving the procedural source. The unrotated,
        // unanchored case has an exact screen-space rectangle; other cases
        // retain the existing renderer path.
        if (node->type == NodeType::Image &&
            node->source.starts_with("procedural:cave:") &&
            !node->attributes.contains("screen-anchor") &&
            std::abs(node->worldTransform().rotation) < 0.001) {
            const Rect bounds=node->bounds();
            const Rect view=renderer_->visibleBounds(display);
            constexpr double fringe=2.0;
            if (bounds.x+bounds.width < view.x-fringe ||
                bounds.y+bounds.height < view.y-fringe ||
                bounds.x > view.x+view.width+fringe ||
                bounds.y > view.y+view.height+fringe) continue;
        }
        const bool direct = node->type == NodeType::ParticleEmitter ||
                            node->type == NodeType::Mesh3D ||
                            node->type == NodeType::Cube3D;
        if (!draw2D && !direct) continue;
        traceNode(*node,false);
        const double originalX=node->transform.position.x;
        const double originalWidth=node->width;
        if (auto anchor=node->attributes.find("screen-anchor");
            anchor!=node->attributes.end()) {
            const Rect original=node->bounds();
            const double fullWidth=node->attributes.contains("screen-base-width")
                ? number(node->attributes.at("screen-base-width")):0.0;
            const Rect anchored=anchoredScreenBounds(original,
                renderer_->visibleBounds(display),anchor->second,fullWidth);
            node->transform.position.x+=anchored.x-original.x;
            node->width+=anchored.width-original.width;
        }
        if (startupTrace_ && node->type==NodeType::Panel && display==Display::Tv)
            startupTrace_("panel-style-begin");
        const ComputedStyle& style = computedStyle(*node);
        if (startupTrace_ && node->type==NodeType::Panel && display==Display::Tv)
            startupTrace_("panel-style-done");
        switch (node->type) {
            case NodeType::Sprite: case NodeType::Image: renderer_->drawSprite(*node, style, imageFor(*node)); break;
            case NodeType::Text: {
                const std::string fontSource = node->attributes.contains("font")
                    ? node->attributes.at("font") : style.fontSource;
                renderer_->drawText(*node, style, fontSource.empty() ? nullptr : &assets_.loadFont(fontSource));
                break;
            }
            case NodeType::Panel: case NodeType::Button: renderer_->drawPanel(*node, style); if (!node->text.empty()) { const Transform world=node->worldTransform(); Node label; label.text=node->text; label.opacity=node->opacity; label.transform.position.x=world.position.x+16; label.transform.position.y=world.position.y+std::max(4.0,(node->height-style.fontSize)/2); const std::string fontSource = node->attributes.contains("font") ? node->attributes.at("font") : style.fontSource; renderer_->drawText(label, style, fontSource.empty() ? nullptr : &assets_.loadFont(fontSource)); } break;
            case NodeType::Ui: case NodeType::SpriteLayer: case NodeType::Group: if (node->width > 0 && node->height > 0) renderer_->drawRectangle(*node, style); break;
            case NodeType::ParticleEmitter: renderer_->drawParticles(*node, style, node->source.empty() ? nullptr : &assets_.loadImage(node->source)); break;
            case NodeType::SpriteBatch: renderer_->drawSpriteBatch(*node, style, imageFor(*node)); break;
            case NodeType::Shape: renderer_->drawShape(*node, style); break;
            case NodeType::Mesh3D: renderer_->drawMesh3D(*node, style, node->source.empty() ? nullptr : &assets_.loadMesh(node->source), camera); break;
            case NodeType::Cube3D: renderer_->drawMesh3D(*node, style, nullptr, camera); break;
            default: break;
        }
        node->transform.position.x=originalX;
        node->width=originalWidth;
        traceNode(*node,true);
    }
    if (draw2D && display == Display::Tv && input_.wiiRemote.pointerValid) {
        Node cursor;
        cursor.width = 14;
        cursor.height = 14;
        cursor.opacity = 1;
        cursor.transform.position.x = input_.wiiRemote.x - 7;
        cursor.transform.position.y = input_.wiiRemote.y - 7;
        ComputedStyle style;
        style.fill = Color{255, 72, 72, 255};
        style.borderWidth = 2;
        style.borderColor = {255, 255, 255, 255};
        style.borderRadius = 7;
        renderer_->drawRectangle(cursor, style);
    }
    renderer_->endDisplay(display);
}

Rect Engine::screenBounds(const Node& node, Display display) const {
    Rect bounds=node.bounds();
    const auto anchor=node.attributes.find("screen-anchor");
    if (anchor==node.attributes.end()) return bounds;
    const double fullWidth=node.attributes.contains("screen-base-width")
        ? number(node.attributes.at("screen-base-width")):0.0;
    return anchoredScreenBounds(bounds,renderer_->visibleBounds(display),
                                anchor->second,fullWidth);
}

void Engine::updateParticles(Node* node, double deltaSeconds) {
    if (!node) return;
    if (node->type == NodeType::ParticleEmitter) {
        auto& emitter = node->emitter;
        for (Particle& particle : emitter.particles) {
            particle.age += deltaSeconds;
            particle.velocity.y += emitter.gravity * deltaSeconds;
            particle.position = particle.position + particle.velocity * deltaSeconds;
            particle.rotation += particle.angularVelocity * deltaSeconds;
        }
        std::erase_if(emitter.particles, [](const Particle& particle) {
            return particle.age >= particle.lifetime;
        });
        if (emitter.emitting) emitter.accumulator += emitter.rate * deltaSeconds;
        std::size_t count = static_cast<std::size_t>(emitter.accumulator) + emitter.pending;
        emitter.accumulator -= std::floor(emitter.accumulator);
        emitter.pending = 0;
        count = std::min(count, emitter.maximum - std::min(emitter.maximum, emitter.particles.size()));
        auto randomUnit = [&]() {
            emitter.randomState = emitter.randomState * 1664525u + 1013904223u;
            return static_cast<double>((emitter.randomState >> 8) & 0xFFFFFFu) / 16777216.0;
        };
        Transform spawnTransform{};
        double spawnCos = 1.0, spawnSin = 0.0;
        if (!emitter.camera && count > 0) {
            spawnTransform = node->worldTransform();
            const double angle = spawnTransform.rotation * std::numbers::pi / 180.0;
            spawnCos = std::cos(angle);
            spawnSin = std::sin(angle);
        }
        for (std::size_t i = 0; i < count; ++i) {
            const double angle = (-90.0 + (randomUnit() - 0.5) * emitter.spread) * std::numbers::pi / 180.0;
            const double speed = emitter.speed * (0.7 + randomUnit() * 0.6);
            Particle particle;
            particle.velocity = {std::cos(angle) * speed, std::sin(angle) * speed, 0};
            if (emitter.spawnWidth > 0.0)
                particle.position.x = (randomUnit() - 0.5) * emitter.spawnWidth;
            particle.camera = emitter.camera;
            if (!particle.camera) {
                const double offsetX = particle.position.x * spawnTransform.scale.x;
                const double offsetY = particle.position.y * spawnTransform.scale.y;
                const double localX = particle.velocity.x * spawnTransform.scale.x;
                const double localY = particle.velocity.y * spawnTransform.scale.y;
                particle.velocity = {localX * spawnCos - localY * spawnSin,
                                     localX * spawnSin + localY * spawnCos, 0};
                particle.position = {spawnTransform.position.x + offsetX * spawnCos - offsetY * spawnSin,
                                     spawnTransform.position.y + offsetX * spawnSin + offsetY * spawnCos, 0};
            }
            particle.lifetime = emitter.lifetime * (0.75 + randomUnit() * 0.5);
            particle.size = emitter.particleSize * (0.75 + randomUnit() * 0.5);
            particle.endSize = emitter.endSize * (0.75 + randomUnit() * 0.5);
            particle.aspect = emitter.aspect;
            particle.rotation = emitter.rotation + (particle.camera ? 0 : spawnTransform.rotation);
            particle.angularVelocity = emitter.angularVelocity;
            particle.color = emitter.startColor;
            particle.endColor = emitter.endColor;
            particle.shape = emitter.shape;
            emitter.particles.push_back(particle);
        }
    }
    for (auto& child : node->children) updateParticles(child.get(), deltaSeconds);
}

void Engine::render() {
    if (!project_) throw std::runtime_error("No project loaded");
    // When both displays contain the same nodes, the GamePad can present the
    // TV's 2D texture. Direct particles and 3D geometry still render per output.
    const bool mirror = !input_.wiiRemote.pointerValid &&
        project_->graph.tvScene() == project_->graph.gamePadScene();
    renderer_->setMirrorDisplays(mirror);
    renderDisplay(Display::Tv);
    renderDisplay(Display::GamePad);
    renderer_->present();
}

std::vector<Node*> Engine::focusableButtons() const {
    auto buttons = project_->graph.buttons(Display::GamePad);
    if (buttons.empty()) buttons = project_->graph.buttons(Display::Tv);
    return buttons;
}

Node* Engine::focusedButton() const {
    if (!project_ || focusedId_.empty()) return nullptr;
    return project_->graph.find(focusedId_);
}

void Engine::focusDefault() {
    if (!project_) return;
    auto buttons = focusableButtons();
    focusedId_ = buttons.empty() ? "" : buttons.front()->id;
    if (Node* node = focusedButton()) node->state = UiState::Focused;
}

void Engine::refreshHover(Display display, double x, double y, bool pressed) {
    if (!project_) return;
    Node* focused = focusedButton();
    for (Node* node : project_->graph.buttons(display)) {
        if (node->disabled) { node->state = UiState::Disabled; continue; }
        if (screenBounds(*node,display).contains(x, y)) node->state = pressed ? UiState::Pressed : UiState::Hover;
        else if (focused == node) node->state = UiState::Focused;
        else node->state = UiState::Normal;
    }
}

void Engine::applyPointerHover() {
    if (!project_) return;
    if (input_.pointerDisplay == Display::GamePad) {
        refreshHover(Display::GamePad, input_.mouse.x, input_.mouse.y, input_.mouse.left.down);
    }
    if (input_.wiiRemote.pointerValid) {
        refreshHover(Display::Tv, input_.wiiRemote.x, input_.wiiRemote.y, input_.wiiRemote.A.down);
    }
}

void Engine::navigateUi() {
    if (!project_) return;
    auto buttons = focusableButtons();
    if (buttons.empty()) {
        focusedId_.clear();
        return;
    }
    Node* current = focusedButton();
    if (!current || std::find(buttons.begin(), buttons.end(), current) == buttons.end()) {
        current = buttons.front();
        focusedId_ = current->id;
        current->state = UiState::Focused;
    }

    int dx = 0, dy = 0;
    if (input_.gamePad.left.pressed) dx = -1;
    if (input_.gamePad.right.pressed) dx = 1;
    if (input_.gamePad.up.pressed) dy = -1;
    if (input_.gamePad.down.pressed) dy = 1;
    if (dx || dy) {
        const Vec2 origin = screenBounds(*current,Display::GamePad).center();
        Node* best = nullptr;
        double bestScore = 1e12;
        for (Node* node : buttons) {
            if (node == current) continue;
            const Vec2 center = screenBounds(*node,Display::GamePad).center();
            const double ox = center.x - origin.x, oy = center.y - origin.y;
            if (dx && ox * dx <= 4) continue;
            if (dy && oy * dy <= 4) continue;
            const double primary = dx ? ox * dx : oy * dy;
            const double secondary = dx ? std::abs(oy) : std::abs(ox);
            const double score = primary + secondary * 2.0;
            if (score < bestScore) { bestScore = score; best = node; }
        }
        if (!best) {
            best = current;
            for (Node* node : buttons) {
                const Vec2 center = screenBounds(*node,Display::GamePad).center();
                const Vec2 bestCenter=screenBounds(*best,Display::GamePad).center();
                if (dx < 0 && center.x > bestCenter.x) best = node;
                if (dx > 0 && center.x < bestCenter.x) best = node;
                if (dy < 0 && center.y > bestCenter.y) best = node;
                if (dy > 0 && center.y < bestCenter.y) best = node;
            }
        }
        if (current->state == UiState::Focused) current->state = UiState::Normal;
        focusedId_ = best->id;
        best->state = UiState::Focused;
        current = best;
    }

    // A scene with its own menu selection model handles A in CafeScript.
    // Keep pointer/touch clicks on its buttons, but avoid activating a
    // different engine-focused button before that script reads GamePad A.
    bool scriptAccept=false;
    if (const Node* scene=project_->graph.sceneFor(Display::GamePad)) {
        const auto mode=scene->attributes.find("controller-accept");
        scriptAccept=mode!=scene->attributes.end() && mode->second=="script";
    }
    if (input_.gamePad.A.pressed && current && script_ && !scriptAccept) {
        current->state = UiState::Pressed;
        script_->emit(current->id, "click");
        current->state = UiState::Focused;
    }
}

void Engine::update(double deltaSeconds) {
    if (!project_) return;
    elapsed_ += deltaSeconds;
#ifdef CAFE_RENDER_PROFILE
    using Clock=std::chrono::steady_clock;
    auto stamp=Clock::now();
    auto elapsedMs=[&] { const auto now=Clock::now(); const double ms=
        std::chrono::duration<double,std::milli>(now-stamp).count(); stamp=now; return ms; };
#endif
    applyPointerHover();
#ifdef CAFE_RENDER_PROFILE
    lastUpdateStats_.hoverMs=elapsedMs();
#endif
    navigateUi();
#ifdef CAFE_RENDER_PROFILE
    lastUpdateStats_.navigationMs=elapsedMs();
#endif
    if (input_.wiiRemote.A.pressed && input_.wiiRemote.pointerValid) {
        clickOn(Display::Tv, input_.wiiRemote.x, input_.wiiRemote.y);
    }
    if (script_) script_->update(deltaSeconds);
#ifdef CAFE_RENDER_PROFILE
    lastUpdateStats_.scriptMs=elapsedMs();
#endif
    updateParticles(&project_->graph.root(), deltaSeconds);
#ifdef CAFE_RENDER_PROFILE
    lastUpdateStats_.particlesMs=elapsedMs();
#endif
    if (!focusedId_.empty() && !project_->graph.find(focusedId_)) focusedId_.clear();
    input_.beginFrame();
}

bool Engine::clickOn(Display display, double x, double y) {
    input_.mouse.x = x;
    input_.mouse.y = y;
    input_.pointerDisplay = display;
    input_.mouse.left = {true, true, false};
    refreshHover(display, x, y, true);
    auto list = project_->graph.renderList(display);
    for (auto it = list.rbegin(); it != list.rend(); ++it) {
        Node* node = *it;
        if (node->type == NodeType::Button && !node->disabled && node->visible &&
            screenBounds(*node,display).contains(x, y)) {
            node->state = UiState::Focused;
            focusedId_ = node->id;
            if (script_) script_->emit(node->id, "click");
            input_.mouse.left = {false, false, true};
            return true;
        }
    }
    input_.mouse.left = {false, false, true};
    return false;
}

void Engine::click(double x, double y) {
    if (!project_) return;
    if (clickOn(Display::GamePad, x, y)) return;
    clickOn(Display::Tv, x, y);
}

void Engine::click(double x, double y, Display display) {
    if (!project_) return;
    clickOn(display, x, y);
}

} // namespace cafe
