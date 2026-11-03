#include "cafe/uml/UmlParser.hpp"
#include "cafe/core/File.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace cafe {

std::string trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return std::string(value);
}

double number(std::string_view value, double fallback) {
    try { return std::stod(std::string(value)); } catch (...) { return fallback; }
}

bool boolean(std::string_view value, bool fallback) {
    if (value == "true" || value == "1") return true;
    if (value == "false" || value == "0") return false;
    return fallback;
}

static int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::optional<Color> Color::parse(std::string_view s) {
    if (s.empty() || s.front() != '#') return std::nullopt;
    s.remove_prefix(1);
    Color out;
    if (s.size() == 3 || s.size() == 4) {
        int v[4]{0, 0, 0, 15};
        for (std::size_t i = 0; i < s.size(); ++i) if ((v[i] = hex(s[i])) < 0) return std::nullopt;
        out = {static_cast<std::uint8_t>(v[0] * 17), static_cast<std::uint8_t>(v[1] * 17),
               static_cast<std::uint8_t>(v[2] * 17), static_cast<std::uint8_t>(v[3] * 17)};
    } else if (s.size() == 6 || s.size() == 8) {
        int v[8]{};
        for (std::size_t i = 0; i < s.size(); ++i) if ((v[i] = hex(s[i])) < 0) return std::nullopt;
        out = {static_cast<std::uint8_t>(v[0] * 16 + v[1]), static_cast<std::uint8_t>(v[2] * 16 + v[3]),
               static_cast<std::uint8_t>(v[4] * 16 + v[5]), static_cast<std::uint8_t>(s.size() == 8 ? v[6] * 16 + v[7] : 255)};
    } else return std::nullopt;
    return out;
}

bool Node::hasClass(std::string_view name) const {
    return std::find(classes.begin(), classes.end(), name) != classes.end();
}

Transform Node::worldTransform() const {
    Transform world = transform;
    if (!parent) return world;
    const Transform ancestor = parent->worldTransform();
    constexpr double pi = 3.14159265358979323846;
    const double radians = ancestor.rotation * pi / 180.0;
    const double localX = transform.position.x * ancestor.scale.x;
    const double localY = transform.position.y * ancestor.scale.y;
    world.position.x = ancestor.position.x + localX * std::cos(radians) - localY * std::sin(radians);
    world.position.y = ancestor.position.y + localX * std::sin(radians) + localY * std::cos(radians);
    world.rotation += ancestor.rotation;
    world.scale.x *= ancestor.scale.x;
    world.scale.y *= ancestor.scale.y;
    return world;
}

Node& Node::add(std::unique_ptr<Node> child) {
    child->parent = this;
    children.push_back(std::move(child));
    return *children.back();
}

SceneGraph::SceneGraph() : root_(std::make_unique<Node>()) {
    root_->type = NodeType::Root; root_->element = "UML";
}

static void indexNode(Node& node, std::unordered_map<std::string, Node*>& index, Node*& firstScene) {
    if (!node.id.empty()) index[node.id] = &node;
    if (!firstScene && (node.type == NodeType::Scene || node.type == NodeType::Scene3D)) firstScene = &node;
    for (auto& child : node.children) indexNode(*child, index, firstScene);
}

static bool underType(const Node* node, NodeType type) {
    for (; node; node = node->parent) if (node->type == type) return true;
    return false;
}

static void collectScenes(Node& node, std::vector<Node*>& scenes) {
    if (node.type == NodeType::Scene || node.type == NodeType::Scene3D) scenes.push_back(&node);
    for (auto& child : node.children) collectScenes(*child, scenes);
}

void SceneGraph::rebuildIndex() {
    const std::string tvId = tvScene_ && !tvScene_->id.empty() ? tvScene_->id : "";
    const std::string padId = gamePadScene_ && !gamePadScene_->id.empty() ? gamePadScene_->id : "";
    byId_.clear();
    Node* firstScene = nullptr;
    indexNode(*root_, byId_, firstScene);

    tvScene_ = nullptr;
    gamePadScene_ = nullptr;
    if (!tvId.empty()) {
        if (Node* node = find(tvId); node &&
            (node->type == NodeType::Scene || node->type == NodeType::Scene3D)) tvScene_ = node;
    }
    if (!padId.empty()) {
        if (Node* node = find(padId); node &&
            (node->type == NodeType::Scene || node->type == NodeType::Scene3D)) gamePadScene_ = node;
    }

    std::vector<Node*> scenes;
    collectScenes(*root_, scenes);
    if (!tvScene_ || !gamePadScene_) {
        for (Node* scene : scenes) {
            if (!tvScene_ && (scene->display == Display::Tv || scene->display == Display::Both)) tvScene_ = scene;
            if (!gamePadScene_ && (scene->display == Display::GamePad || scene->display == Display::Both)) gamePadScene_ = scene;
        }
    }
    if (!tvScene_) tvScene_ = firstScene;
    if (!gamePadScene_) gamePadScene_ = firstScene;
    activeScene_ = tvScene_ ? tvScene_ : gamePadScene_;
}

Node* SceneGraph::find(std::string_view id) const {
    auto found = byId_.find(std::string(id));
    return found == byId_.end() ? nullptr : found->second;
}

Node* SceneGraph::sceneFor(Display display) const {
    if (display == Display::GamePad) return gamePadScene_;
    return tvScene_;
}

bool SceneGraph::openScene(std::string_view id) {
    return openScene(id, Display::Both);
}

bool SceneGraph::openScene(std::string_view id, Display display) {
    Node* node = find(id);
    if (!node || (node->type != NodeType::Scene && node->type != NodeType::Scene3D)) return false;
    if (display == Display::Tv || display == Display::Both) tvScene_ = node;
    if (display == Display::GamePad || display == Display::Both) gamePadScene_ = node;
    activeScene_ = tvScene_ ? tvScene_ : gamePadScene_;
    return true;
}

static void flatten(Node& node, std::vector<Node*>& out) {
    for (auto& child : node.children) {
        out.push_back(child.get());
        flatten(*child, out);
    }
}

std::vector<Node*> SceneGraph::renderList(Display display) const {
    std::vector<Node*> result;
    Node* scene = sceneFor(display);
    if (!scene) return result;
    std::vector<Node*> layers;
    for (auto& child : scene->children) layers.push_back(child.get());
    std::stable_sort(layers.begin(), layers.end(), [](const Node* a, const Node* b) { return a->order < b->order; });
    for (Node* layer : layers) { result.push_back(layer); flatten(*layer, result); }
    return result;
}

std::vector<Node*> SceneGraph::buttons(Display display) const {
    std::vector<Node*> result;
    for (Node* node : renderList(display)) {
        if (node->type == NodeType::Button && node->visible && !node->disabled) result.push_back(node);
    }
    return result;
}

std::string SceneGraph::nextId(std::string_view base) {
    std::string name = base.empty() ? "node" : std::string(base);
    for (;;) {
        std::string id = name + "_" + std::to_string(spawnSerial_++);
        if (!find(id)) return id;
    }
}

std::unique_ptr<Node> SceneGraph::cloneTree(const Node& source) {
    auto node = std::make_unique<Node>();
    node->type = source.type;
    node->element = source.element;
    node->classes = source.classes;
    node->transform = source.transform;
    node->transform3D = source.transform3D;
    node->visible = source.visible;
    node->opacity = source.opacity;
    node->width = source.width;
    node->height = source.height;
    node->depth = source.depth;
    node->fieldOfView = source.fieldOfView;
    node->nearClip = source.nearClip;
    node->farClip = source.farClip;
    node->order = source.order;
    node->text = source.text;
    node->source = source.source;
    node->tint = source.tint;
    node->hue = source.hue;
    node->saturation = source.saturation;
    node->state = UiState::Normal;
    node->disabled = source.disabled;
    node->display = source.display;
    node->attributes = source.attributes;
    node->emitter = source.emitter;
    node->emitter.particles.clear();
    node->emitter.accumulator = 0.0;
    node->emitter.pending = 0;
    node->batch = source.batch;
    node->batch.revision = 0;
    node->id = nextId(source.id.empty() ? source.element : source.id);
    for (auto& child : source.children) node->add(cloneTree(*child));
    return node;
}

Node* SceneGraph::spawn(std::string_view templateId, std::string_view parentId, double x, double y) {
    Node* proto = find(templateId);
    if (!proto) return nullptr;
    Node* parent = parentId.empty() ? nullptr : find(parentId);
    if (!parent) {
        Node* scene = tvScene_ ? tvScene_ : gamePadScene_;
        if (scene) {
            for (auto& child : scene->children) {
                if (child->type == NodeType::SpriteLayer) { parent = child.get(); break; }
            }
            if (!parent) parent = scene;
        }
    }
    if (!parent || underType(parent, NodeType::Templates)) return nullptr;
    auto cloned = cloneTree(*proto);
    cloned->transform.position.x = x;
    cloned->transform.position.y = y;
    cloned->transform3D.position.x = x;
    cloned->transform3D.position.y = y;
    Node& added = parent->add(std::move(cloned));
    rebuildIndex();
    return &added;
}

bool SceneGraph::destroy(std::string_view id) {
    Node* node = find(id);
    if (!node || !node->parent || node->type == NodeType::Scene ||
        node->type == NodeType::Scene3D || node->type == NodeType::Templates) return false;
    if (underType(node, NodeType::Templates)) return false;
    auto& siblings = node->parent->children;
    auto it = std::find_if(siblings.begin(), siblings.end(), [node](const std::unique_ptr<Node>& child) { return child.get() == node; });
    if (it == siblings.end()) return false;
    siblings.erase(it);
    rebuildIndex();
    return true;
}

bool SceneGraph::overlaps(const Node& a, const Node& b) {
    return a.bounds().overlaps(b.bounds());
}

std::string_view nodeTypeName(NodeType type) {
    static constexpr std::string_view names[]{
        "UML", "main", "scene", "spritelayer", "sprite", "image", "ui",
        "panel", "text", "button", "group", "templates", "scene3d",
        "camera", "mesh", "cube", "particles", "audio", "spritebatch", "shape"
    };
    return names[static_cast<std::size_t>(type)];
}

namespace {
struct XmlTag { std::string name; std::unordered_map<std::string, std::string> attrs; bool closing{false}, selfClosing{false}; };

NodeType nodeType(std::string_view name) {
    if (name == "main") return NodeType::Main;
    if (name == "scene") return NodeType::Scene;
    if (name == "scene3d") return NodeType::Scene3D;
    if (name == "camera" || name == "camera3d") return NodeType::Camera3D;
    if (name == "mesh" || name == "mesh3d") return NodeType::Mesh3D;
    if (name == "cube" || name == "cube3d") return NodeType::Cube3D;
    if (name == "particles" || name == "particle-emitter") return NodeType::ParticleEmitter;
    if (name == "audio" || name == "sound") return NodeType::Audio;
    if (name == "spritebatch" || name == "sprite-batch") return NodeType::SpriteBatch;
    if (name == "shape" || name == "circle" || name == "ellipse" || name == "line" ||
        name == "polygon" || name == "arc") return NodeType::Shape;
    if (name == "spritelayer") return NodeType::SpriteLayer;
    if (name == "sprite") return NodeType::Sprite;
    if (name == "image") return NodeType::Image;
    if (name == "ui") return NodeType::Ui;
    if (name == "panel") return NodeType::Panel;
    if (name == "text") return NodeType::Text;
    if (name == "button") return NodeType::Button;
    if (name == "templates") return NodeType::Templates;
    return NodeType::Group;
}

XmlTag parseTag(std::string_view raw) {
    XmlTag tag; raw = std::string_view(raw.data(), raw.size());
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) raw.remove_prefix(1);
    if (!raw.empty() && raw.front() == '/') { tag.closing = true; raw.remove_prefix(1); }
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) raw.remove_suffix(1);
    if (!raw.empty() && raw.back() == '/') { tag.selfClosing = true; raw.remove_suffix(1); }
    std::size_t p = 0; while (p < raw.size() && !std::isspace(static_cast<unsigned char>(raw[p]))) ++p;
    tag.name = std::string(raw.substr(0, p));
    while (p < raw.size()) {
        while (p < raw.size() && std::isspace(static_cast<unsigned char>(raw[p]))) ++p;
        std::size_t start = p; while (p < raw.size() && raw[p] != '=' && !std::isspace(static_cast<unsigned char>(raw[p]))) ++p;
        if (start == p) break;
        std::string key(raw.substr(start, p - start));
        while (p < raw.size() && (std::isspace(static_cast<unsigned char>(raw[p])) || raw[p] == '=')) ++p;
        if (p >= raw.size() || (raw[p] != '"' && raw[p] != '\'')) break;
        char quote = raw[p++]; start = p; while (p < raw.size() && raw[p] != quote) ++p;
        tag.attrs[key] = std::string(raw.substr(start, p - start)); if (p < raw.size()) ++p;
    }
    return tag;
}

void assign(Node& node, const std::unordered_map<std::string, std::string>& attrs) {
    node.attributes = attrs;
    auto get = [&](std::string_view k) -> std::string { auto it = attrs.find(std::string(k)); return it == attrs.end() ? "" : it->second; };
    node.id = get("id"); node.source = get("src");
    if (node.type == NodeType::Shape && !attrs.contains("kind")) node.attributes["kind"] = node.element;
    if (auto tint = Color::parse(get("tint"))) node.tint = *tint;
    node.hue = number(get("hue"),0);
    node.saturation = number(get("saturation"),1);
    node.transform.position = {number(get("x")), number(get("y"))};
    node.transform.rotation = number(get("rotation"));
    double commonScale = number(get("scale"), 1); node.transform.scale = {number(get("scale-x"), commonScale), number(get("scale-y"), commonScale)};
    node.transform3D.position = {number(get("x")), number(get("y")), number(get("z"))};
    node.transform3D.rotation = {number(get("rotation-x")), number(get("rotation-y")),
                                 number(get("rotation-z"), node.transform.rotation)};
    node.transform3D.scale = {number(get("scale-x"), commonScale),
                              number(get("scale-y"), commonScale),
                              number(get("scale-z"), commonScale)};
    node.width = number(get("width")); node.height = number(get("height"));
    node.depth = number(get("depth"), node.width > 0 ? node.width : 1.0);
    node.fieldOfView = number(get("fov"), 60.0);
    node.nearClip = number(get("near"), 0.1);
    node.farClip = number(get("far"), 1000.0);
    node.order = attrs.contains("order") ? static_cast<int>(number(get("order"))) : (node.type == NodeType::Ui ? 100 : 0);
    node.opacity = number(get("opacity"), 1); node.visible = boolean(get("visible"), true); node.disabled = boolean(get("disabled"), false);
    if (node.type == NodeType::SpriteBatch)
        node.batch.maximum = static_cast<std::size_t>(std::max(1.0, number(get("max"), 8192.0)));
    node.display = parseDisplay(get("display"));
    if (node.type == NodeType::ParticleEmitter) {
        node.emitter.camera = boolean(get("camera"), true);
        node.emitter.rate = number(get("rate"), 20.0);
        node.emitter.lifetime = number(get("lifetime"), 1.0);
        node.emitter.speed = number(get("speed"), 60.0);
        node.emitter.spread = number(get("spread"), 45.0);
        node.emitter.spawnWidth = std::max(0.0, number(get("spawn-width"), 0.0));
        node.emitter.gravity = number(get("gravity"), 0.0);
        node.emitter.particleSize = number(get("particle-size"), number(get("size"), 4.0));
        node.emitter.endSize = number(get("end-size"), node.emitter.particleSize);
        const double particleWidth = number(get("particle-width"), node.emitter.particleSize);
        const double particleHeight = number(get("particle-height"), node.emitter.particleSize);
        node.emitter.particleSize = particleHeight;
        node.emitter.aspect = particleHeight > 0 ? particleWidth / particleHeight : 1.0;
        node.emitter.rotation = number(get("particle-rotation"), 0.0);
        node.emitter.angularVelocity = number(get("spin"), number(get("angular-velocity"), 0.0));
        node.emitter.maximum = static_cast<std::size_t>(std::max(1.0, number(get("max"), 256.0)));
        node.emitter.emitting = boolean(get("emitting"), true);
        const std::string startColor = !get("start-color").empty() ? get("start-color") : get("color");
        const std::string endColor = get("end-color");
        if (auto parsed = Color::parse(startColor)) node.emitter.startColor = *parsed;
        if (auto parsed = Color::parse(endColor)) node.emitter.endColor = *parsed;
        if (get("shape") == "rectangle" || get("shape") == "rect" || get("shape") == "square")
            node.emitter.shape = ParticleShape::Rectangle;
    }
    std::istringstream classes(get("class")); for (std::string c; classes >> c;) node.classes.push_back(c);
}
}

UmlResult UmlParser::parse(std::string_view source, const std::filesystem::path& base) const {
    UmlResult result; std::vector<Node*> stack{&result.graph.root()}; std::size_t pos = 0, textStart = 0;
    while (pos < source.size()) {
        auto open = source.find('<', pos); if (open == std::string_view::npos) break;
        if (open > textStart && !stack.empty()) {
            auto content = trim(source.substr(textStart, open - textStart));
            if (!content.empty() && stack.back()->type != NodeType::Root) stack.back()->text += content;
        }
        if (source.substr(open, 4) == "<!--") { auto end = source.find("-->", open + 4); if (end == std::string_view::npos) throw std::runtime_error("Unclosed UML comment"); pos = textStart = end + 3; continue; }
        auto close = source.find('>', open + 1); if (close == std::string_view::npos) throw std::runtime_error("Unclosed UML tag");
        XmlTag tag = parseTag(source.substr(open + 1, close - open - 1));
        if (tag.closing) { if (stack.size() > 1) stack.pop_back(); }
        else if (tag.name == "style" || tag.name == "script") {
            const bool isStyle = tag.name == "style";
            auto it = tag.attrs.find("src");
            if (it != tag.attrs.end())
                (isStyle ? result.links.style : result.links.script) = base / it->second;
            if (!tag.selfClosing) {
                const std::string closingTag = isStyle ? "</style>" : "</script>";
                const auto end = source.find(closingTag, close + 1);
                if (end == std::string_view::npos)
                    throw std::runtime_error(std::string("Unclosed inline ") + tag.name + " block");
                std::string& inlineSource = isStyle
                    ? result.links.inlineStyle : result.links.inlineScript;
                if (!inlineSource.empty()) inlineSource.push_back('\n');
                inlineSource.append(source.substr(close + 1, end - close - 1));
                pos = textStart = end + closingTag.size();
                continue;
            }
        } else if (tag.name != "UML" && !tag.name.empty()) {
            auto node = std::make_unique<Node>(); node->element = tag.name; node->type = nodeType(tag.name); assign(*node, tag.attrs);
            Node& added = stack.back()->add(std::move(node)); if (!tag.selfClosing) stack.push_back(&added);
        }
        pos = textStart = close + 1;
    }
    result.graph.rebuildIndex();
    return result;
}

UmlResult UmlParser::parseFile(const std::filesystem::path& path) const {
    return parse(readTextFile(path), path.parent_path());
}

} // namespace cafe
