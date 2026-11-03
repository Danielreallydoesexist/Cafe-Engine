#include "cafe/core/Engine.hpp"
#include "cafe/renderer/Renderer.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#if defined(CAFE_DESKTOP_WINDOW)
#include "cafe/platform/DesktopWindow.hpp"
#endif

namespace {

struct Options {
    std::filesystem::path project{"examples/hello_world/main.uml"};
    std::filesystem::path output{"cafe-preview.ppm"};
    bool preview{false};
    std::optional<std::pair<double, double>> click;
};

Options parseArgs(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--preview" || arg == "--once") options.preview = true;
        else if (arg == "--click" && i + 2 < argc) {
            options.click = {std::stod(argv[++i]), std::stod(argv[++i])};
            options.preview = true;
        } else if (arg == "--output" && i + 1 < argc) {
            options.output = argv[++i];
            options.preview = true;
        } else if (!arg.empty() && arg.front() != '-') {
            options.project = arg;
        }
    }
    return options;
}

int runPreview(const Options& options) {
    auto renderer = std::make_unique<cafe::SoftwareRenderer>(640, 480, options.output);
    cafe::Engine engine(std::move(renderer));
    engine.loadProject(options.project);
    engine.update(1.0 / 60.0);
    if (options.click) engine.click(options.click->first, options.click->second);
    engine.update(1.0 / 60.0);
    engine.render();
    std::cout << "Cafe Engine rendered " << options.project << " to " << options.output << "\n";
    if (auto* scene = engine.sceneGraph().tvScene()) std::cout << "TV scene: " << scene->id << "\n";
    if (auto* scene = engine.sceneGraph().gamePadScene()) std::cout << "GamePad scene: " << scene->id << "\n";
    return 0;
}

#if defined(CAFE_DESKTOP_WINDOW)
int runWindow(const Options& options) {
    auto renderer = std::make_unique<cafe::SoftwareRenderer>(640, 480);
    cafe::SoftwareRenderer* software = renderer.get();
    cafe::Engine engine(std::move(renderer));
    engine.loadProject(options.project);

    cafe::DesktopWindow window;
    if (!window.open(software->width(), software->height())) {
        std::cerr << "Cafe Engine: could not open a desktop window\n";
        return 1;
    }

    std::cout << "Cafe Engine desktop loop — TV left, GamePad right. Close the window to quit.\n";
    std::cout << "Mouse/keyboard: Wii Remote (TV). Xbox controller: GamePad (right pane).\n";

    using Clock = std::chrono::steady_clock;
    auto last = Clock::now();
    while (window.poll(engine.input())) {
        const auto now = Clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;
        dt = std::clamp(dt, 0.0, 0.05);
        if (engine.input().mouse.left.released && engine.input().pointerDisplay == cafe::Display::GamePad) {
            engine.click(engine.input().mouse.x, engine.input().mouse.y, cafe::Display::GamePad);
        }
        engine.update(dt);
        engine.render();
        window.present(software->pixels(cafe::Display::Tv), software->pixels(cafe::Display::GamePad),
                       software->width(), software->height());
        const auto frameEnd = Clock::now();
        const auto elapsed = frameEnd - last;
        constexpr auto targetFrame = std::chrono::microseconds(33333);
        if (elapsed < targetFrame) {
            std::this_thread::sleep_for(targetFrame - elapsed);
        }
    }
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parseArgs(argc, argv);
#if defined(CAFE_DESKTOP_WINDOW)
        if (!options.preview) return runWindow(options);
#endif
        return runPreview(options);
    } catch (const std::exception& error) {
        std::cerr << "Cafe Engine: " << error.what() << '\n';
        return 1;
    }
}
