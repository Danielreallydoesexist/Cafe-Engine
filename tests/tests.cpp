#include "cafe/cafescript/Interpreter.hpp"
#include "cafe/core/Engine.hpp"
#include "cafe/cstyle/Style.hpp"
#include "cafe/renderer/Font.hpp"
#include "cafe/renderer/Renderer.hpp"
#include "cafe/uml/UmlParser.hpp"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

int main() {
    cafe::AssetManager media;
    media.setRoot(std::filesystem::path(CAFE_SOURCE_DIR) / "examples/hello_world");
    const auto& png = media.loadImage("assets/player.png");
    const auto& svg = media.loadImage("assets/badge.svg");
    const auto& wav = media.loadAudio("assets/chime.wav");
    const auto& ogg = media.loadAudio("assets/chime.ogg");
    const auto& smoothMesh = media.loadMesh("assets/smooth.obj");
    assert(png.available && png.width == 64 && png.height == 64 && !png.pixels.empty());
    assert(svg.available && svg.width == 128 && svg.height == 128 && !svg.pixels.empty());
    assert(wav.available && wav.sampleRate == 48000 && wav.channels == 2 && !wav.samples.empty());
    assert(ogg.available && ogg.sampleRate > 0 && ogg.channels == 2 && !ogg.samples.empty());
    assert(smoothMesh.available && smoothMesh.vertices.size() == 6);
    std::vector<cafe::Vec3> sharedNormals;
    for (const auto& vertex : smoothMesh.vertices)
        if (std::abs(vertex.position.x + 0.8) < 0.001 &&
            std::abs(vertex.position.y + 0.8) < 0.001 &&
            std::abs(vertex.position.z) < 0.001) sharedNormals.push_back(vertex.normal);
    assert(sharedNormals.size() == 2);
    assert(cafe::dot(sharedNormals[0], sharedNormals[1]) > 0.999);
    assert(sharedNormals[0].y > 0.5 && sharedNormals[0].z > 0.5);

    const cafe::Rect wideViewport = cafe::aspectFitViewport(640, 480, 1280, 720);
    assert(std::abs(wideViewport.x - 160.0) < 0.001);
    assert(std::abs(wideViewport.y) < 0.001);
    assert(std::abs(wideViewport.width - 960.0) < 0.001);
    assert(std::abs(wideViewport.height - 720.0) < 0.001);
    const auto wideCenter = cafe::outputToVirtual(wideViewport, 640, 480, 640, 360);
    assert(wideCenter && std::abs(wideCenter->x - 320.0) < 0.001 &&
           std::abs(wideCenter->y - 240.0) < 0.001);
    assert(!cafe::outputToVirtual(wideViewport, 640, 480, 20, 360));
    const cafe::Rect gamePadViewport = cafe::aspectFitViewport(640, 480, 854, 480);
    assert(std::abs(gamePadViewport.x - 107.0) < 0.001);
    const auto gamePadOrigin = cafe::outputToVirtual(gamePadViewport, 640, 480, 107, 0);
    assert(gamePadOrigin && std::abs(gamePadOrigin->x) < 0.001 &&
           std::abs(gamePadOrigin->y) < 0.001);
    assert(!cafe::outputToVirtual(gamePadViewport, 640, 480, 106, 240));
    const cafe::Rect tallViewport = cafe::aspectFitViewport(640, 480, 400, 900);
    assert(std::abs(tallViewport.x) < 0.001);
    assert(std::abs(tallViewport.y - 300.0) < 0.001);
    assert(std::abs(tallViewport.width - 400.0) < 0.001);
    assert(std::abs(tallViewport.height - 300.0) < 0.001);
    const auto tallCenter = cafe::outputToVirtual(tallViewport, 640, 480, 200, 450);
    assert(tallCenter && std::abs(tallCenter->x - 320.0) < 0.001 &&
           std::abs(tallCenter->y - 240.0) < 0.001);
    assert(!cafe::outputToVirtual(tallViewport, 640, 480, 200, 299));
    const cafe::Rect tinyViewport = cafe::aspectFitViewport(640, 480, 2, 2);
    assert(tinyViewport.width > 0.0 && tinyViewport.height > 0.0);
    assert(cafe::aspectFitViewport(0, 480, 800, 600).width == 0.0);
    const cafe::Rect expandedTv = cafe::expandedLogicalBounds(640,480,1280,720);
    const cafe::Rect expandedPad = cafe::expandedLogicalBounds(640,480,854,480);
    assert(std::abs(expandedTv.width - 853.3333333333) < 0.001);
    assert(std::abs(expandedTv.x + 106.6666666667) < 0.001);
    assert(std::abs(expandedPad.width - 854.0) < 0.001);
    assert(std::abs(expandedPad.x + 107.0) < 0.001);
    const cafe::Rect edgeBag=cafe::anchoredScreenBounds(
        {520,408,110,62},expandedPad,"right");
    assert(std::abs(edgeBag.x-627.0)<0.001);
    assert(std::abs(expandedPad.x+expandedPad.width-
                    (edgeBag.x+edgeBag.width)-10.0)<0.001);
    const cafe::Rect wideHud=cafe::anchoredScreenBounds(
        {12,10,616,57},expandedPad,"stretch");
    assert(std::abs(wideHud.x+95.0)<0.001 &&
           std::abs(wideHud.width-830.0)<0.001);
    const cafe::Rect halfBar=cafe::anchoredScreenBounds(
        {27,52,289,10},expandedPad,"stretch-progress",578);
    assert(std::abs(halfBar.x+80.0)<0.001 &&
           std::abs(halfBar.width-396.0)<0.001);
    assert(std::abs(cafe::expandedLogicalBounds(640,480,640,480).width-640)<0.001);
    assert(cafe::expandedLogicalBounds(640,480,0,480).width==0.0);
    struct WideUiRenderer : cafe::SoftwareRenderer {
        WideUiRenderer() : SoftwareRenderer(640,480) {}
        double buttonX{-1};
        bool pendingResize{false};
        bool tvNarrow{false};
        bool refreshOutputGeometry() override {
            if (!pendingResize) return false;
            pendingResize=false;tvNarrow=true;return true;
        }
        [[nodiscard]] cafe::Rect visibleBounds(cafe::Display display) const override {
            if (tvNarrow && display==cafe::Display::Tv) return {0,0,640,480};
            return {-107,0,854,480};
        }
        void drawPanel(const cafe::Node& node,
                       const cafe::ComputedStyle& style) override {
            if (node.id=="edge") buttonX=node.bounds().x;
            SoftwareRenderer::drawPanel(node,style);
        }
    };
    const auto uiTestFile=std::filesystem::temp_directory_path()/
        "cafe-responsive-ui-test.uml";
    {
        std::ofstream testProject(uiTestFile);
        testProject << R"(<UML><main><scene id="ui">
          <button id="edge" screen-anchor="right" x="520" y="20"
                  width="110" height="40">BAG</button>
          </scene><script>
          function update(dt) { edge.text = String(Screen.tvWidth); }
          #edge on click { edge.text = "clicked"; }
          </script>
          </main></UML>)";
    }
    auto wideRenderer=std::make_unique<WideUiRenderer>();
    WideUiRenderer* wideRendererView=wideRenderer.get();
    cafe::Engine wideUi(std::move(wideRenderer));
    wideUi.loadProject(uiTestFile);
    wideUi.render();
    assert(std::abs(wideRendererView->buttonX-627.0)<0.001);
    wideRendererView->pendingResize=true;
    assert(wideUi.refreshDisplayLayout());
    wideUi.update(1.0/60.0);
    assert(wideUi.sceneGraph().find("edge")->text=="640");
    assert(!wideUi.refreshDisplayLayout());
    wideUi.click(640,30,cafe::Display::GamePad);
    assert(wideUi.sceneGraph().find("edge")->text=="clicked");
    std::filesystem::remove(uiTestFile);
    const auto scriptAcceptFile=std::filesystem::temp_directory_path()/
        "cafe-script-accept-test.uml";
    {
        std::ofstream project(scriptAcceptFile);
        project << R"(<UML><main>
          <scene id="saves" controller-accept="script">
            <text id="result"></text>
            <button id="selected" disabled="true" x="0" y="0" width="80" height="40">ONE</button>
            <button id="other" x="100" y="0" width="80" height="40">TWO</button>
          </scene><script>
          function update(dt) { if(GamePad.pressed.A && result.text=="") result.text="selected"; }
          #other on click { result.text="wrong"; }
          </script></main></UML>)";
    }
    cafe::Engine scriptedAccept(std::make_unique<cafe::SoftwareRenderer>(640,480));
    scriptedAccept.loadProject(scriptAcceptFile);
    scriptedAccept.input().gamePad.A.pressed=true;
    scriptedAccept.update(1.0/60.0);
    assert(scriptedAccept.sceneGraph().find("result")->text=="selected");
    scriptedAccept.sceneGraph().find("result")->text="";
    scriptedAccept.click(120,20,cafe::Display::GamePad);
    assert(scriptedAccept.sceneGraph().find("result")->text=="wrong");
    std::filesystem::remove(scriptAcceptFile);
    const cafe::DualPaneLayout oddPanes = cafe::splitDisplayPanes(801, 601);
    assert(std::abs(oddPanes.left.width - 399.5) < 0.001);
    assert(std::abs(oddPanes.divider.x - 399.5) < 0.001);
    assert(std::abs(oddPanes.divider.width - 2.0) < 0.001);
    assert(std::abs(oddPanes.right.x - 401.5) < 0.001);
    assert(std::abs(oddPanes.right.width - 399.5) < 0.001);
    assert(std::abs(oddPanes.right.x + oddPanes.right.width - 801.0) < 0.001);
    const cafe::DualPaneLayout tinyPanes = cafe::splitDisplayPanes(1, 10);
    assert(tinyPanes.left.width == 0.0 && tinyPanes.divider.width == 1.0 &&
           tinyPanes.right.width == 0.0);

    auto parsed = cafe::UmlParser{}.parse(R"(<UML><main>
      <templates><sprite id="spark" x="0" y="0" width="10" height="10"/></templates>
      <scene id="world" display="tv"><spritelayer id="layer"><sprite id="p" x="0" y="0" width="10" height="10"/></spritelayer></scene>
      <scene id="hud" display="gamepad"><ui id="u" order="5"><button id="b" class="primary" x="0" y="0" width="20" height="20">Go</button><button id="c" x="40" y="0" width="20" height="20">Hi</button></ui></scene>
    </main></UML>)");
    assert(parsed.graph.find("p") && parsed.graph.find("p")->transform.position.x == 0);
    assert(parsed.graph.tvScene() && parsed.graph.tvScene()->id == "world");
    assert(parsed.graph.gamePadScene() && parsed.graph.gamePadScene()->id == "hud");
    assert(parsed.graph.renderList(cafe::Display::Tv).front()->id == "layer");
    assert(parsed.graph.renderList(cafe::Display::GamePad).front()->id == "u");

    auto embedded = cafe::UmlParser{}.parse(R"(<UML><main>
      <scene id="inline-scene"><ui><button id="inline-button" class="inline" width="20" height="20">Inline</button></ui></scene>
    </main>
    <style src="base.cstyle">
      .inline { fill: linear-gradient(90deg, rgba(255,0,0,0.5), #0000ff00); blend-mode: screen; }
    </style>
    <script>
      let inlineValue = 0;
      if (1 < 2) { inlineValue = 42; }
    </script></UML>)", "/project");
    assert(embedded.links.style == std::filesystem::path("/project/base.cstyle"));
    assert(embedded.links.inlineStyle.find("blend-mode: screen") != std::string::npos);
    assert(embedded.links.inlineScript.find("1 < 2") != std::string::npos);
    cafe::StyleSheet inlineSheet;
    inlineSheet.parse(embedded.links.inlineStyle);
    assert(inlineSheet.compute(*embedded.graph.find("inline-button")).blendMode == cafe::BlendMode::Screen);
    cafe::InputSystem inlineInput;
    cafe::script::Interpreter inlineScript(embedded.graph, inlineInput);
    inlineScript.load(embedded.links.inlineScript);
    assert(inlineScript.global("inlineValue").asNumber() == 42);

    auto* spark = parsed.graph.spawn("spark", "layer", 5, 6);
    assert(spark && spark->transform.position.x == 5);
    assert(cafe::SceneGraph::overlaps(*spark, *parsed.graph.find("p")));
    const std::string sparkId = spark->id;
    assert(parsed.graph.destroy(sparkId));
    assert(!parsed.graph.find(sparkId));
    assert(!parsed.graph.destroy("spark"));

    auto hierarchy = cafe::UmlParser{}.parse(R"(<UML><main><scene id="camera-scene">
      <group id="camera-group" x="10" y="20" scale="2">
        <sprite id="world-object" x="3" y="4" width="5" height="6"/>
      </group></scene></main></UML>)");
    const cafe::Rect worldBounds = hierarchy.graph.find("world-object")->bounds();
    assert(std::abs(worldBounds.x - 16) < 0.001 && std::abs(worldBounds.y - 28) < 0.001);
    assert(std::abs(worldBounds.width - 10) < 0.001 && std::abs(worldBounds.height - 12) < 0.001);

    auto filteredScene=cafe::UmlParser{}.parse(R"(<UML><main><scene id="filters">
      <image id="electric" x="0" y="0" width="10" height="10"
             hue="190" saturation="1.45" />
      </scene></main></UML>)");
    auto* electric=filteredScene.graph.find("electric");
    assert(electric && electric->hue==190 && electric->saturation==1.45);
    cafe::InputSystem filterInput;
    cafe::script::Interpreter filterScript(filteredScene.graph,filterInput);
    filterScript.load("electric.hue=120;electric.saturation=1;");
    assert(electric->hue==120 && electric->saturation==1);
    cafe::Asset redPixel{};
    redPixel.type=cafe::AssetType::Image;
    redPixel.available=true;
    redPixel.width=redPixel.height=1;
    redPixel.pixels={{255,0,0,255}};
    cafe::SoftwareRenderer filteredPreview(20,20);
    filteredPreview.beginDisplay(cafe::Display::Tv,{0,0,0,255});
    filteredPreview.drawSprite(*electric,cafe::ComputedStyle{},&redPixel);
    const auto filteredColor=filteredPreview.pixels(cafe::Display::Tv)[5*20+5];
    assert(filteredColor.g>230 && filteredColor.r<20 && filteredColor.b<20);

    cafe::StyleSheet sheet; sheet.parse("button { fill: #123; } .primary { border-radius: 9px; blend-mode: screen; shading: smooth; } button:hover { fill: #abcdef88; }");
    auto* button = parsed.graph.find("b"); auto normal = sheet.compute(*button); assert(normal.fill && normal.borderRadius == 9);
    assert(normal.blendMode == cafe::BlendMode::Screen);
    assert(normal.shadingMode == cafe::ShadingMode::Gouraud);
    button->state = cafe::UiState::Hover; auto hover = sheet.compute(*button); assert(std::get<cafe::Color>(*hover.fill).a == 0x88);

    cafe::InputSystem input; cafe::script::Interpreter script(parsed.graph, input);
    script.setScreenBounds(expandedTv,expandedPad);
    script.load(R"(
      let n = 0;
      let screenLeft = Screen.left;
      let screenWidth = Screen.width;
      let screenRef = Screen;
      let last = "";
      let sawPad = false;
      function update(dt) {
        p.x += dt * 10;
        if (GamePad.up && GamePad.pressed.left) sawPad = true;
      }
      #b on click { n += 1; b.text = "Done"; }
      #c on click {
        if (last) destroy(last);
        let spark = spawn("spark", "layer", 8, 1);
        last = spark.id;
      }
    )");
    assert(std::abs(script.global("screenLeft").asNumber()+107.0)<0.001);
    assert(std::abs(script.global("screenWidth").asNumber()-854.0)<0.001);
    script.setScreenBounds({0,0,640,480},expandedPad);
    const auto screenRef=std::get<std::shared_ptr<cafe::script::Object>>(
        script.global("screenRef").data);
    assert(screenRef->fields.at("tvWidth").asNumber()==640.0);
    input.gamePad.up.down = true;
    input.gamePad.left.pressed = true;
    script.update(.5);
    assert(parsed.graph.find("p")->transform.position.x == 5);
    assert(script.global("sawPad").truthy());
    input.wiiRemote.A.down = true;
    input.wiiRemote.pointerValid = true;
    input.wiiRemote.x = 42;
    cafe::script::Interpreter remote(parsed.graph, input);
    remote.load("let pointed=false; function update(dt){ if (WiiRemote.A && WiiRemote.valid && WiiRemote.x > 40) pointed=true; }");
    remote.update(0);
    assert(remote.global("pointed").truthy());
    script.emit("b","click");
    assert(button->text == "Done");
    assert(script.global("n").asNumber() == 1);
    script.emit("c","click");
    assert(script.global("last").asString().find("spark_") == 0);
    assert(parsed.graph.find(script.global("last").asString()));
    script.emit("c","click");
    assert(parsed.graph.find(script.global("last").asString()));

    cafe::script::Interpreter es6(parsed.graph, input);
    es6.load(R"(
      const values = [1, 2, 3, 4];
      const doubled = values.map(value => value * 2);
      let total = 0;
      for (const value of doubled) {
        if (value === 4) continue;
        total += value;
        if (total > 10) break;
      }
      const label = `total=${total}`;
      const verdict = total > 10 ? "fast" : "slow";
      const x = 3;
      const point = {x, y: 4};
      const pointSum = point.x + point.y;
      let indexed = [1];
      indexed[2] = 9;
      indexed[0] *= 2;
      const joined = indexed.filter(value => value > 1).join("-");
      const square = (value => value ** 2)(3);
      const add = function(a, b) { return a + b; };
      const mathValue = Math.clamp(add(square, pointSum), 0, 15);
      function makeAdder(base) { return value => base + value; }
      const addFive = makeAdder(5);
      const closureValue = addFive(4);
      function makeCounter() {
        let count = 0;
        const first = () => ++count;
        const second = () => ++count;
        count = 4;
        return {first, second};
      }
      const counter = makeCounter();
      const firstCount = counter.first();
      const secondCount = counter.second();
      function makeBlockCounter() {
        if (true) {
          let count = 10;
          return () => ++count;
        }
      }
      const blockCounter = makeBlockCounter();
      const blockFirst = blockCounter();
      for (let i = 0; i < 30; i++) { let scratch = i; }
      const blockSecond = blockCounter();
      let letters = "";
      for (const letter of "JS") letters += letter;
      let prefix = 1;
      const prefixValue = ++prefix;
      const strictTypes = 1 === "1";
      const nullIsUndefined = null === undefined;
      const kind = typeof doubled;
      let conditionalSteps = [];
      if (true) {
        conditionalSteps.push("first");
        conditionalSteps.push("second");
      }
      let conditionalValue = 0;
      if (false) {
        conditionalValue = 100;
        conditionalValue += 100;
      } else if (true) {
        conditionalValue = 3;
        conditionalValue += 4;
      } else {
        conditionalValue = -1;
      }
      const conditionalResult = conditionalSteps.join("-");
      const reduced = values.reduce((sum, value) => sum + value, 0);
      const found = values.find(value => value > 2);
      const allPositive = values.every(value => value > 0);
      const words = " cafe,engine ".trim().split(",");
      const copied = Object.assign({}, {ready: true});
      const objectReady = copied.ready;
    )");
    assert(es6.global("total").asNumber() == 16);
    assert(es6.global("label").asString() == "total=16");
    assert(es6.global("verdict").asString() == "fast");
    assert(es6.global("pointSum").asNumber() == 7);
    assert(es6.global("joined").asString() == "2-9");
    assert(es6.global("square").asNumber() == 9);
    assert(es6.global("mathValue").asNumber() == 15);
    assert(es6.global("closureValue").asNumber() == 9);
    assert(es6.global("firstCount").asNumber() == 5);
    assert(es6.global("secondCount").asNumber() == 6);
    assert(es6.global("blockFirst").asNumber() == 11);
    assert(es6.global("blockSecond").asNumber() == 12);
    assert(es6.global("letters").asString() == "JS");
    assert(es6.global("prefixValue").asNumber() == 2);
    assert(!es6.global("strictTypes").truthy());
    assert(!es6.global("nullIsUndefined").truthy());
    assert(es6.global("kind").asString() == "object");
    assert(es6.global("conditionalResult").asString() == "first-second");
    assert(es6.global("conditionalValue").asNumber() == 7);
    assert(es6.global("reduced").asNumber() == 10);
    assert(es6.global("found").asNumber() == 3);
    assert(es6.global("allPositive").truthy());
    assert(es6.global("objectReady").truthy());
    bool constRejected = false;
    try {
        cafe::script::Interpreter invalid(parsed.graph, input);
        invalid.load("const locked = 1; locked = 2;");
    } catch (const std::runtime_error&) {
        constRejected = true;
    }
    assert(constRejected);

    auto compatibilityGraph = cafe::UmlParser{}.parse(R"(<UML><main><scene id="compat">
      <spritelayer><spritebatch id="fragments" src="assets/player.png" width="8" height="8" max="7000" cache="true"/>
      <shape id="warning-ring" kind="circle" x="20" y="20" width="30" height="30"/>
    </spritelayer></scene></main></UML>)");
    auto* fragments = compatibilityGraph.graph.find("fragments");
    assert(fragments && fragments->type == cafe::NodeType::SpriteBatch &&
           fragments->batch.maximum == 7000);
    std::unordered_map<std::string, std::string> savedFiles;
    std::string lastAudioAction;
    double lastAudioValue = 0;
    cafe::InputSystem compatibilityInput;
    compatibilityInput.keyboard.keys[static_cast<unsigned char>('r')].down = true;
    compatibilityInput.keyboard.keys[static_cast<std::size_t>(cafe::KeyCode::Escape)].pressed = true;
    compatibilityInput.mouse.left.pressed = true;
    cafe::script::Interpreter compatibility(compatibilityGraph.graph, compatibilityInput);
    compatibility.setStorageCallbacks(
        [&](std::string_view key) { return savedFiles[std::string(key)]; },
        [&](std::string_view key, std::string_view value) {
            savedFiles[std::string(key)] = value; return true;
        },
        [&](std::string_view key) { return savedFiles.contains(std::string(key)); },
        [&](std::string_view key) { return savedFiles.erase(std::string(key)) > 0; });
    compatibility.setAudioPlayCallback([](std::string_view, bool, double) { return 17; });
    compatibility.setAudioControlCallback([&](int voice, std::string_view action, double value) {
        assert(voice == 17); lastAudioAction = action; lastAudioValue = value;
    });
    compatibility.setAudioStateCallback([](int voice) { return voice == 17; });
    compatibility.setAlphaHitCallback([](std::string_view source, double x, double y, double threshold) {
        return source == "mask.png" && x == 4 && y == 5 && threshold < 0.5;
    });
    compatibility.load(R"(
      const wrote = Storage.write("slot-1.txt", "quests=4");
      const restored = Storage.read("slot-1.txt", "missing");
      const voice = Audio.play("music.ogg", {loop: true, volume: 0.5});
      Audio.setPitch(voice, 1.25);
      const playing = Audio.isPlaying(voice);
      const spriteIndex = fragments.add({x: 3, y: 4, width: 12, height: 14,
        rotation: 20, opacity: 0.7, tint: "#ff80ffff"});
      fragments.set(spriteIndex, {x: 9});
      const bulkCount = setSprites(fragments, [
        {x: 9, y: 4, width: 12, height: 14, tint: "#ff80ffff"},
        {x: 30, y: 8, width: 8, height: 8}
      ]);
      const maskHit = Collision.alpha("mask.png", 4, 5, 0.1);
      let timerResult = 0;
      setTimeout(() => { timerResult = 9; }, 100);
      let keyboardWorked = false;
      let pointerWorked = false;
      function update(dt) {
        if (Keyboard.r && Keyboard.pressed.escape) keyboardWorked = true;
        if (Mouse.pressed.left && Mouse.display === "gamepad") pointerWorked = true;
      }
    )");
    compatibility.update(0.05);
    assert(compatibility.global("timerResult").asNumber() == 0);
    compatibility.update(0.06);
    assert(compatibility.global("timerResult").asNumber() == 9);
    assert(compatibility.global("wrote").truthy());
    assert(compatibility.global("restored").asString() == "quests=4");
    assert(compatibility.global("playing").truthy());
    assert(compatibility.global("maskHit").truthy());
    assert(compatibility.global("keyboardWorked").truthy());
    assert(compatibility.global("pointerWorked").truthy());
    assert(lastAudioAction == "pitch" && std::abs(lastAudioValue - 1.25) < 0.001);
    assert(compatibility.global("bulkCount").asNumber() == 2);
    assert(fragments->batch.sprites.size() == 2 && fragments->batch.sprites[0].x == 9);

    cafe::SoftwareRenderer renderer(160, 64);
    renderer.beginDisplay(cafe::Display::Tv, {0, 0, 0, 255});
    cafe::Node label;
    label.text = "Hi";
    label.transform.position = {8, 12};
    cafe::ComputedStyle text;
    text.fontSize = 28;
    text.textColor = {255, 255, 255, 255};
    renderer.drawText(label, text);
    int lit = 0;
    for (const auto& color : renderer.pixels(cafe::Display::Tv)) if (color.r > 40) ++lit;
    assert(cafe::EmbeddedFont::instance().ready());
    assert(lit > 40);

    cafe::AssetManager projectFonts;
    projectFonts.setRoot(CAFE_SOURCE_DIR);
    const auto& externalFont = projectFonts.loadFont("third_party/fonts/AtkinsonHyperlegible-Regular.ttf");
    assert(externalFont.available && !externalFont.bytes.empty());
    renderer.beginDisplay(cafe::Display::Tv, {0, 0, 0, 255});
    renderer.drawText(label, text, &externalFont);
    int projectFontPixels = 0;
    for (const auto& color : renderer.pixels(cafe::Display::Tv)) if (color.r > 40) ++projectFontPixels;
    assert(projectFontPixels > 40);

    cafe::SoftwareRenderer compatibilityDrawing(96, 64);
    compatibilityDrawing.beginDisplay(cafe::Display::Tv, {0, 0, 0, 255});
    cafe::ComputedStyle batchStyle;
    batchStyle.fill = cafe::Color{220, 80, 150, 255};
    compatibilityDrawing.drawSpriteBatch(*fragments, batchStyle, &png);
    // A second draw reuses the static raster rather than walking every sprite.
    compatibilityDrawing.drawSpriteBatch(*fragments, batchStyle, &png);
    cafe::Node circle;
    circle.type = cafe::NodeType::Shape;
    circle.attributes["kind"] = "circle";
    circle.transform.position = {50, 12};
    circle.width = circle.height = 32;
    cafe::ComputedStyle circleStyle;
    circleStyle.fill = cafe::RadialGradient{{{0, {255,255,255,255}}, {1, {120,30,255,0}}}};
    circleStyle.borderWidth = 2;
    circleStyle.borderColor = {255,255,255,255};
    compatibilityDrawing.drawShape(circle, circleStyle);
    const auto& compatibilityPixels = compatibilityDrawing.pixels(cafe::Display::Tv);
    assert(compatibilityPixels[static_cast<std::size_t>(10 * 96 + 12)].r > 100);
    assert(compatibilityPixels[static_cast<std::size_t>(28 * 96 + 66)].r > 100);

    cafe::SoftwareRenderer effects(112, 96);
    effects.beginDisplay(cafe::Display::Tv, {0, 0, 0, 255});
    cafe::Node card;
    card.transform.position = {24, 20};
    card.width = 64;
    card.height = 48;
    cafe::ComputedStyle decorated;
    decorated.fill = cafe::LinearGradient{90, {{0, {220, 60, 40, 255}}, {1, {40, 90, 230, 255}}}};
    decorated.borderWidth = 2;
    decorated.borderColor = {255, 255, 255, 255};
    decorated.borderRadius = 10;
    decorated.sawtooth = cafe::Sawtooth{"bottom", 12, 10};
    decorated.shadows.push_back({0, 2, 8, {0, 0, 0, 190}, true});
    effects.drawPanel(card, decorated);
    const auto& effectPixels = effects.pixels(cafe::Display::Tv);
    auto at = [&](int x, int y) -> cafe::Color { return effectPixels[static_cast<std::size_t>(y * effects.width() + x)]; };
    assert(at(24, 20).r == 0 && at(24, 20).g == 0 && at(24, 20).b == 0); // rounded corner clips
    assert(at(40, 71).r + at(40, 71).g + at(40, 71).b > 0); // gradient/border reaches a tooth
    assert(at(56, 23).r + at(56, 23).g + at(56, 23).b < at(56, 44).r + at(56, 44).g + at(56, 44).b); // rounded inset shades inward

    auto sideHasTeeth = [](std::string side, int sampleX, int sampleY) {
        cafe::SoftwareRenderer sideRenderer(64, 64);
        sideRenderer.beginDisplay(cafe::Display::Tv, {0, 0, 0, 255});
        cafe::Node shape;
        shape.transform.position = {20, 20};
        shape.width = 24;
        shape.height = 24;
        cafe::ComputedStyle style;
        style.fill = cafe::Color{200, 100, 50, 255};
        style.sawtooth = cafe::Sawtooth{std::move(side), 10, 7};
        sideRenderer.drawRectangle(shape, style);
        const auto color = sideRenderer.pixels(cafe::Display::Tv)[static_cast<std::size_t>(sampleY * 64 + sampleX)];
        return color.r > 0;
    };
    assert(sideHasTeeth("top", 25, 16));
    assert(sideHasTeeth("bottom", 25, 47));
    assert(sideHasTeeth("left", 16, 25));
    assert(sideHasTeeth("right", 47, 25));

    auto alphaGradient = cafe::parsePaint("linear-gradient(90deg, rgba(255,0,0,0.25), #0000ff00)");
    assert(alphaGradient && std::holds_alternative<cafe::LinearGradient>(*alphaGradient));
    const auto& alphaStops = std::get<cafe::LinearGradient>(*alphaGradient).stops;
    assert(alphaStops.front().color.a >= 63 && alphaStops.front().color.a <= 64);
    assert(alphaStops.back().color.a == 0);

    cafe::SoftwareRenderer traced(80, 72);
    traced.beginDisplay(cafe::Display::Tv, {0, 0, 0, 255});
    cafe::Node ticket;
    ticket.transform.position = {20, 16};
    ticket.width = 40;
    ticket.height = 28;
    cafe::ComputedStyle ticketStyle;
    ticketStyle.fill = cafe::Color{190, 45, 35, 255};
    ticketStyle.borderWidth = 3;
    ticketStyle.borderColor = {255, 255, 255, 255};
    ticketStyle.borderRadius = 8;
    ticketStyle.sawtooth = cafe::Sawtooth{"bottom", 10, 8};
    traced.drawRectangle(ticket, ticketStyle);
    const auto& tracedPixels = traced.pixels(cafe::Display::Tv);
    auto tracedAt = [&](int x, int y) -> cafe::Color {
        return tracedPixels[static_cast<std::size_t>(y * traced.width() + x)];
    };
    // The fill crosses the old rectangle/tooth seam; only the actual zig-zag
    // perimeter is white.
    assert(tracedAt(33, 43).r > tracedAt(33, 43).g * 2);
    assert(tracedAt(33, 50).r > 220 && tracedAt(33, 50).g > 220);
    assert(tracedAt(20, 16).r < 20); // rounded corner remains outside

    cafe::SoftwareRenderer blurredShadow(80, 64);
    blurredShadow.beginDisplay(cafe::Display::Tv, {0, 0, 0, 255});
    cafe::Node shadowCard;
    shadowCard.transform.position = {30, 20};
    shadowCard.width = 20;
    shadowCard.height = 20;
    cafe::ComputedStyle shadowStyle;
    shadowStyle.fill = cafe::Color{40, 50, 60, 255};
    shadowStyle.borderRadius = 6;
    shadowStyle.shadows.push_back({0, 0, 10, {255, 255, 255, 255}, false});
    blurredShadow.drawRectangle(shadowCard, shadowStyle);
    const auto& shadowPixels = blurredShadow.pixels(cafe::Display::Tv);
    auto shadowAt = [&](int x, int y) -> cafe::Color {
        return shadowPixels[static_cast<std::size_t>(y * blurredShadow.width() + x)];
    };
    assert(shadowAt(27, 30).r > shadowAt(22, 30).r);
    assert(shadowAt(22, 30).r > 0);

    cafe::SoftwareRenderer transparentLayer(32, 32);
    transparentLayer.beginDisplay(cafe::Display::Tv, {0, 0, 0, 0});
    cafe::Node translucentPanel;
    translucentPanel.transform.position = {4, 4};
    translucentPanel.width = translucentPanel.height = 20;
    cafe::ComputedStyle translucentStyle;
    translucentStyle.fill = cafe::Color{250, 100, 40, 128};
    transparentLayer.drawRectangle(translucentPanel, translucentStyle);
    const auto coverage = transparentLayer.pixels(cafe::Display::Tv)[10*32+10];
    assert(coverage.a >= 127 && coverage.a <= 129);
    assert(coverage.r >= 249); // Straight alpha is retained for GX2 compositing.

    cafe::AssetManager caves;
    const auto& curved=caves.loadImage("procedural:cave:0.33:-0.28:5:1:0:1");
    assert(curved.available && curved.width==768 && curved.height==768);
    assert(curved.pixels[384*768+384].a==0);
    assert(curved.pixels[384*768+8].a==255);
    const auto& rounded=caves.loadImage("procedural:cave:0:0:0:0:1:0");
    assert(rounded.pixels[0].a==0); // Open biome corner is rounded.
    assert(rounded.pixels[164*768+164].a==255);
    const auto& squareOverride=caves.loadImage(
        "procedural:cave:0:0:0:0:1:0;roundedCorners=false");
    assert(squareOverride.available && squareOverride.pixels[0].a==255);
    const auto& roundOverride=caves.loadImage(
        "procedural:cave:0:0:0:0:0:0;roundedCorners=true");
    assert(roundOverride.available && roundOverride.pixels[0].a==0);
    assert(!caves.loadImage(
        "procedural:cave:0:0:0:0:0:0;roundedCorners=maybe").available);
    const auto& scaled=caves.loadImage(
        "procedural:cave:0:0:5:0:0:0;width=256;height=384;edge=outline;edgeColor=#878787");
    assert(scaled.available && scaled.width==256 && scaled.height==384);
    assert(scaled.pixels[192*256+128].a==0);
    assert(scaled.pixels[192*256+4].a==255);
    assert(!caves.loadImage("procedural:cave:0:0:0").available);
    const cafe::Asset* firstSlot=nullptr;
    for (int i=0;i<25;++i) {
        const std::string key="procedural:cave:"+std::to_string(i/100.0)+":0:0:0:0:0";
        const cafe::Asset& image=caves.loadImage(key);
        if (i==0) firstSlot=&image;
        if (i==24) assert(image.revision>1); // The 24-slot cache reused one allocation.
    }
    assert(firstSlot && firstSlot->pixels.size()==768*768);
    assert(caves.get("procedural:cave:0.33:-0.28:5:1:0:1")==nullptr);

    cafe::AssetManager procedural;
    const auto& noise=procedural.loadImage(
        "procedural:simplex:seed=42;width=96;height=72;scale=28;octaves=3;"
        "low=#183250;high=#e4b481");
    assert(noise.available && noise.width==96 && noise.height==72);
    assert(&noise==&procedural.loadImage(
        "procedural:simplex:seed=42;width=96;height=72;scale=28;octaves=3;"
        "low=#183250;high=#e4b481"));
    assert(noise.pixels[0].a==255 && noise.pixels[0].r!=noise.pixels[100].r);
    const auto& terrain=procedural.loadImage(
        "procedural:terrain:width=128;height=128;seed=5;surface=0.35;"
        "caveScale=16;caveThreshold=0.3;solid=#171729;edge=stones;"
        "edgeColor=#e0d5ba");
    assert(terrain.available && terrain.pixels[0].a==0);
    std::size_t solidCount=0,emptyCount=0,edgeCount=0;
    for (const auto& pixel:terrain.pixels) {
        solidCount+=pixel.a==255;
        emptyCount+=pixel.a==0;
        edgeCount+=pixel.r==0xe0 && pixel.g==0xd5;
    }
    assert(solidCount>1000 && emptyCount>1000 && edgeCount>0);
    const auto& mask=procedural.loadImage(
        "procedural:mask:shape=ellipse;width=128;height=128;seed=6;"
        "roughness=0.1;solid=#334455;edge=outline;edgeColor=#ffffff;edgeWidth=3");
    assert(mask.available && mask.pixels[0].a==0 &&
           mask.pixels[64*128+64].a==255);
    const auto& squareMask=procedural.loadImage(
        "procedural:mask:shape=rectangle;width=128;height=128;"
        "roughness=0;solid=#334455;roundedCorners=false");
    const auto& roundedMask=procedural.loadImage(
        "procedural:mask:shape=rectangle;width=128;height=128;"
        "roughness=0;solid=#334455;roundedCorners=true;cornerRadius=24");
    assert(squareMask.available && roundedMask.available);
    assert(squareMask.pixels[14*128+14].a==255);
    assert(roundedMask.pixels[14*128+14].a==0);
    assert(roundedMask.pixels[64*128+64].a==255);
    const auto placement=procedural.edgePoints(
        "procedural:mask:shape=ellipse;width=128;height=128;seed=6;"
        "roughness=0.1;solid=#334455;edge=outline;edgeColor=#ffffff;edgeWidth=3",12);
    assert(placement.size()>8 && placement.size()<128);
    for (const auto& point:placement) {
        assert(point.x>=0 && point.x<=1 && point.y>=0 && point.y<=1);
        assert(std::abs(std::hypot(point.nx,point.ny)-1)<0.001);
    }
    cafe::SceneGraph maskGraph;
    cafe::InputSystem maskInput;
    cafe::script::Interpreter maskScript(maskGraph,maskInput);
    maskScript.setMaskEdgesCallback([&](std::string_view src,int spacing,double threshold) {
        return procedural.edgePoints(src,spacing,threshold);
    });
    maskScript.load("let placements = Mask.edgePoints(\"procedural:mask:shape=ellipse;width=128;height=128;seed=6;roughness=0.1;solid=#334455;edge=outline;edgeColor=#ffffff;edgeWidth=3\",12);");
    const auto scripted=maskScript.global("placements");
    assert(std::get<std::shared_ptr<cafe::script::Value::Array>>(scripted.data)->size()==placement.size());
    assert(!procedural.loadImage("procedural:simplex:width=99999").available);

    cafe::SoftwareRenderer scene3d(160, 120);
    scene3d.beginDisplay(cafe::Display::Tv, {0, 0, 0, 255});
    cafe::Node camera;
    camera.type = cafe::NodeType::Camera3D;
    camera.transform3D.position.z = -5;
    cafe::Node cube;
    cube.type = cafe::NodeType::Cube3D;
    cube.transform3D.position.z = 1;
    cube.width = cube.height = cube.depth = 1.5;
    cafe::ComputedStyle cubeStyle;
    cubeStyle.fill = cafe::Color{80, 180, 255, 255};
    scene3d.drawMesh3D(cube, cubeStyle, nullptr, &camera);
    int threeDimensionalPixels = 0;
    for (const auto& color : scene3d.pixels(cafe::Display::Tv))
        if (color.b > 40) ++threeDimensionalPixels;
    assert(threeDimensionalPixels > 100);

    cafe::Node emitter;
    emitter.type = cafe::NodeType::ParticleEmitter;
    emitter.transform.position = {80, 60};
    cafe::Particle testParticle;
    testParticle.lifetime = 1;
    testParticle.size = 8;
    testParticle.endSize = 8;
    testParticle.color = {255, 200, 20, 255};
    emitter.emitter.particles.push_back(testParticle);
    scene3d.drawParticles(emitter, cafe::ComputedStyle{}, nullptr);

    cafe::SoftwareRenderer radialParticles(64, 64);
    radialParticles.beginDisplay(cafe::Display::Tv, {0, 0, 0, 255});
    cafe::Node radialEmitter;
    radialEmitter.type = cafe::NodeType::ParticleEmitter;
    radialEmitter.transform.position = {32, 32};
    cafe::Particle radialParticle;
    radialParticle.lifetime = 1;
    radialParticle.size = radialParticle.endSize = 20;
    radialParticle.color = {255, 255, 255, 255};
    radialParticle.endColor = {255, 255, 255, 255};
    radialEmitter.emitter.particles.push_back(radialParticle);
    cafe::ComputedStyle radialStyle;
    radialStyle.fill = cafe::RadialGradient{{
        {0, {255, 255, 255, 255}}, {1, {255, 255, 255, 0}}
    }};
    radialParticles.drawParticles(radialEmitter, radialStyle, nullptr);
    const auto& radialPixels = radialParticles.pixels(cafe::Display::Tv);
    auto radialAt = [&](int x, int y) -> cafe::Color {
        return radialPixels[static_cast<std::size_t>(y * radialParticles.width() + x)];
    };
    assert(radialAt(32, 32).r > 220);
    assert(radialAt(41, 32).r < radialAt(32, 32).r / 2);

    std::cout << "Cafe Engine tests passed\n";
}
