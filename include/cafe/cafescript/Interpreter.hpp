#pragma once

#include "cafe/core/Node.hpp"
#include "cafe/input/Input.hpp"
#include "cafe/assets/AssetManager.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace cafe::script {

struct Object;
struct Function;
struct NullValue {};
struct Value {
    using Array = std::vector<Value>;
    using Storage = std::variant<std::monostate, NullValue, double, std::string, bool,
        std::shared_ptr<Array>, std::shared_ptr<Object>, std::shared_ptr<Function>, Node*>;
    Storage data;
    Value() = default;
    Value(double v) : data(v) {}
    Value(std::string v) : data(std::move(v)) {}
    Value(const char* v) : data(std::string(v)) {}
    Value(bool v) : data(v) {}
    Value(std::nullptr_t) : data(NullValue{}) {}
    Value(Node* v) : data(v) {}
    [[nodiscard]] bool truthy() const;
    [[nodiscard]] double asNumber() const;
    [[nodiscard]] std::string asString() const;
};

struct Object { std::unordered_map<std::string, Value> fields; };

struct Expr;
struct Stmt;
struct Function {
    std::vector<std::string> params;
    std::vector<std::shared_ptr<Stmt>> body;
    std::function<Value(const std::vector<Value>&)> native;
    std::shared_ptr<void> closure;
};

class Interpreter {
public:
    using EventCallback = std::function<void(std::string_view)>;
    using NodeCallback = std::function<void(Node*)>;
    using AudioPlayCallback = std::function<int(std::string_view, bool, double)>;
    using AudioStopCallback = std::function<void(int)>;
    using AudioControlCallback = std::function<void(int, std::string_view, double)>;
    using AudioStateCallback = std::function<bool(int)>;
    using StorageReadCallback = std::function<std::string(std::string_view)>;
    using StorageWriteCallback = std::function<bool(std::string_view, std::string_view)>;
    using StorageActionCallback = std::function<bool(std::string_view)>;
    using AlphaHitCallback = std::function<bool(std::string_view, double, double, double)>;
    using MaskEdgesCallback = std::function<std::vector<MaskEdgePoint>(
        std::string_view,int,double)>;
    Interpreter(SceneGraph& graph, InputSystem& input);
    ~Interpreter();
    void load(std::string_view source);
    void runLoad();
    void update(double deltaSeconds);
    void emit(std::string_view nodeId, std::string_view event);
    void setSceneOpenCallback(EventCallback callback) { sceneOpen_ = std::move(callback); }
    void setSpawnCallback(NodeCallback callback) { spawnHook_ = std::move(callback); }
    void setAudioPlayCallback(AudioPlayCallback callback) { audioPlay_ = std::move(callback); }
    void setAudioStopCallback(AudioStopCallback callback) { audioStop_ = std::move(callback); }
    void setAudioControlCallback(AudioControlCallback callback) { audioControl_ = std::move(callback); }
    void setAudioStateCallback(AudioStateCallback callback) { audioState_ = std::move(callback); }
    void setStorageCallbacks(StorageReadCallback read, StorageWriteCallback write,
                             StorageActionCallback exists, StorageActionCallback remove) {
        storageRead_ = std::move(read); storageWrite_ = std::move(write);
        storageExists_ = std::move(exists); storageRemove_ = std::move(remove);
    }
    void setAlphaHitCallback(AlphaHitCallback callback) { alphaHit_ = std::move(callback); }
    void setMaskEdgesCallback(MaskEdgesCallback callback) { maskEdges_ = std::move(callback); }
    void setScreenBounds(Rect tv, Rect gamePad);
    [[nodiscard]] Value global(std::string_view name) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    EventCallback sceneOpen_;
    NodeCallback spawnHook_;
    AudioPlayCallback audioPlay_;
    AudioStopCallback audioStop_;
    AudioControlCallback audioControl_;
    AudioStateCallback audioState_;
    StorageReadCallback storageRead_;
    StorageWriteCallback storageWrite_;
    StorageActionCallback storageExists_;
    StorageActionCallback storageRemove_;
    AlphaHitCallback alphaHit_;
    MaskEdgesCallback maskEdges_;
};

} // namespace cafe::script
