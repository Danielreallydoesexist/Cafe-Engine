#pragma once

#include "cafe/core/Types.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace cafe {

enum class NodeType {
    Root, Main, Scene, SpriteLayer, Sprite, Image, Ui, Panel, Text, Button,
    Group, Templates, Scene3D, Camera3D, Mesh3D, Cube3D, ParticleEmitter, Audio,
    SpriteBatch, Shape
};
enum class UiState { Normal, Hover, Pressed, Focused, Disabled };
enum class ParticleShape { Circle, Rectangle };

struct SpriteInstance {
    double x{0}, y{0};
    double width{0}, height{0};
    double rotation{0};
    double scaleX{1}, scaleY{1};
    double opacity{1};
    Color tint{255, 255, 255, 255};
    bool visible{true};
};

struct SpriteBatchState {
    std::vector<SpriteInstance> sprites;
    std::size_t maximum{8192};
    std::uint64_t revision{0};
};

struct Particle {
    Vec3 position{};
    Vec3 velocity{};
    bool camera{true};
    double age{0.0};
    double lifetime{1.0};
    double size{4.0};
    double endSize{4.0};
    double aspect{1.0};
    double rotation{0.0};
    double angularVelocity{0.0};
    Color color{255, 255, 255, 255};
    Color endColor{255, 255, 255, 0};
    ParticleShape shape{ParticleShape::Circle};
};

struct ParticleEmitterState {
    std::vector<Particle> particles;
    bool camera{true};
    double rate{20.0};
    double lifetime{1.0};
    double speed{60.0};
    double spread{45.0};
    double spawnWidth{0.0};
    double gravity{0.0};
    double particleSize{4.0};
    double endSize{4.0};
    double aspect{1.0};
    double rotation{0.0};
    double angularVelocity{0.0};
    Color startColor{255, 210, 80, 255};
    Color endColor{255, 80, 20, 0};
    ParticleShape shape{ParticleShape::Circle};
    std::size_t maximum{256};
    double accumulator{0.0};
    std::size_t pending{0};
    std::uint32_t randomState{0xC0FFEEu};
    bool emitting{true};
};

struct Node {
    NodeType type{NodeType::Group};
    std::string element{"group"};
    std::string id;
    std::vector<std::string> classes;
    Node* parent{nullptr};
    std::vector<std::unique_ptr<Node>> children;
    Transform transform{};
    Transform3D transform3D{};
    bool visible{true};
    double opacity{1.0};
    double width{0.0};
    double height{0.0};
    double depth{1.0};
    double fieldOfView{60.0};
    double nearClip{0.1};
    double farClip{1000.0};
    int order{0};
    std::string text;
    std::string source;
    Color tint{255, 255, 255, 255};
    double hue{0.0};
    double saturation{1.0};
    UiState state{UiState::Normal};
    bool disabled{false};
    Display display{Display::Both};
    std::unordered_map<std::string, std::string> attributes;
    ParticleEmitterState emitter{};
    SpriteBatchState batch{};

    [[nodiscard]] Transform worldTransform() const;
    [[nodiscard]] Rect bounds() const {
        const Transform world = worldTransform();
        return {world.position.x, world.position.y,
                width * std::abs(world.scale.x), height * std::abs(world.scale.y)};
    }
    [[nodiscard]] bool hasClass(std::string_view name) const;
    Node& add(std::unique_ptr<Node> child);
};

class SceneGraph {
public:
    SceneGraph();
    Node& root() { return *root_; }
    const Node& root() const { return *root_; }
    Node* find(std::string_view id) const;
    Node* activeScene() const { return tvScene_ ? tvScene_ : gamePadScene_; }
    Node* tvScene() const { return tvScene_; }
    Node* gamePadScene() const { return gamePadScene_; }
    Node* sceneFor(Display display) const;
    bool openScene(std::string_view id);
    bool openScene(std::string_view id, Display display);
    void rebuildIndex();
    std::vector<Node*> renderList() const { return renderList(Display::Tv); }
    std::vector<Node*> renderList(Display display) const;
    std::vector<Node*> buttons(Display display) const;
    Node* spawn(std::string_view templateId, std::string_view parentId, double x, double y);
    bool destroy(std::string_view id);
    [[nodiscard]] static bool overlaps(const Node& a, const Node& b);

private:
    std::unique_ptr<Node> root_;
    std::unordered_map<std::string, Node*> byId_;
    Node* activeScene_{nullptr};
    Node* tvScene_{nullptr};
    Node* gamePadScene_{nullptr};
    int spawnSerial_{1};

    std::string nextId(std::string_view base);
    std::unique_ptr<Node> cloneTree(const Node& source);
};

std::string_view nodeTypeName(NodeType type);

} // namespace cafe
