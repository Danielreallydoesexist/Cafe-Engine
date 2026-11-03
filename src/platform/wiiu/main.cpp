#include "cafe/core/Engine.hpp"
#include "cafe/core/File.hpp"
#include "cafe/renderer/WiiURenderer.hpp"

#include <coreinit/debug.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <padscore/kpad.h>
#include <vpad/input.h>
#include <whb/proc.h>
#include <whb/sdcard.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {


void setButton(cafe::ButtonState& target, std::uint32_t held,
               std::uint32_t pressed, std::uint32_t released,
               std::uint32_t mask) {
    target.down = (held & mask) != 0;
    target.pressed = (pressed & mask) != 0;
    target.released = (released & mask) != 0;
}

void configureRemote(KPADChan chan) {
    WPADExtensionType extension = WPAD_EXT_CORE;
    if (WPADProbe(chan, &extension) != WPAD_ERROR_NONE) return;
    const WPADDataFormat format =
        (extension == WPAD_EXT_NUNCHUK || extension == WPAD_EXT_MPLUS_NUNCHUK)
            ? WPAD_FMT_NUNCHUK_ACC_DPD
            : WPAD_FMT_CORE_ACC_DPD;
    if (WPADGetDataFormat(chan) != format) WPADSetDataFormat(chan, format);
    KPADEnableDPD(chan);
}

void onRemoteConnect(WPADChan chan, WPADError status) {
    if (status == WPAD_ERROR_NONE) configureRemote(static_cast<KPADChan>(chan));
}

class WiiUInputAdapter {
public:
    explicit WiiUInputAdapter(const cafe::WiiURenderer& renderer) : renderer_(renderer) {
        if (!WPADGetStatus()) WPADInit();
        KPADInit();
        WPADEnableWiiRemote(TRUE);
        WPADEnableURCC(TRUE);
        for (int chan = 0; chan < 4; ++chan) {
            KPADSetConnectCallback(static_cast<KPADChan>(chan), onRemoteConnect);
            configureRemote(static_cast<KPADChan>(chan));
        }
        ready_ = true;
    }

    ~WiiUInputAdapter() {
        if (!ready_) return;
        KPADShutdown();
        if (WPADGetStatus()) WPADShutdown();
    }

    void poll(cafe::Engine& engine) {
        auto& input = engine.input();
        pollGamePad(engine, input);
        pollWiiRemote(input);
    }

private:
    const cafe::WiiURenderer& renderer_;
    bool ready_{false};
    bool rawWasTouching_{false};
    bool touchStartedInside_{false};
    bool wasTouchActive_{false};
    double lastTouchX_{0.0};
    double lastTouchY_{0.0};

    void pollGamePad(cafe::Engine& engine, cafe::InputSystem& input) {
        VPADStatus status{};
        VPADReadError error = VPAD_READ_SUCCESS;
        if (VPADRead(VPAD_CHAN_0, &status, 1, &error) <= 0 || error != VPAD_READ_SUCCESS) return;

        setButton(input.gamePad.A, status.hold, status.trigger, status.release, VPAD_BUTTON_A);
        setButton(input.gamePad.B, status.hold, status.trigger, status.release, VPAD_BUTTON_B);
        setButton(input.gamePad.X, status.hold, status.trigger, status.release, VPAD_BUTTON_X);
        setButton(input.gamePad.Y, status.hold, status.trigger, status.release, VPAD_BUTTON_Y);
        setButton(input.gamePad.L, status.hold, status.trigger, status.release, VPAD_BUTTON_L);
        setButton(input.gamePad.R, status.hold, status.trigger, status.release, VPAD_BUTTON_R);
        setButton(input.gamePad.ZL, status.hold, status.trigger, status.release, VPAD_BUTTON_ZL);
        setButton(input.gamePad.ZR, status.hold, status.trigger, status.release, VPAD_BUTTON_ZR);
        setButton(input.gamePad.plus, status.hold, status.trigger, status.release, VPAD_BUTTON_PLUS);
        setButton(input.gamePad.minus, status.hold, status.trigger, status.release, VPAD_BUTTON_MINUS);
        setButton(input.gamePad.up, status.hold, status.trigger, status.release,
                  VPAD_BUTTON_UP | VPAD_STICK_L_EMULATION_UP);
        setButton(input.gamePad.down, status.hold, status.trigger, status.release,
                  VPAD_BUTTON_DOWN | VPAD_STICK_L_EMULATION_DOWN);
        setButton(input.gamePad.left, status.hold, status.trigger, status.release,
                  VPAD_BUTTON_LEFT | VPAD_STICK_L_EMULATION_LEFT);
        setButton(input.gamePad.right, status.hold, status.trigger, status.release,
                  VPAD_BUTTON_RIGHT | VPAD_STICK_L_EMULATION_RIGHT);
        input.gamePad.leftStick.x = status.leftStick.x;
        input.gamePad.leftStick.y = status.leftStick.y;
        input.gamePad.rightStick.x = status.rightStick.x;
        input.gamePad.rightStick.y = status.rightStick.y;

        VPADTouchData touch{};
        VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &touch, &status.tpNormal);
        const bool rawTouching = touch.touched != 0 && touch.validity == VPAD_VALID;
        std::optional<cafe::Vec2> logicalPoint;
        if (rawTouching) {
            const double outputX = static_cast<double>(touch.x) *
                renderer_.outputWidth(cafe::Display::GamePad) / 854.0;
            const double outputY = static_cast<double>(touch.y) *
                renderer_.outputHeight(cafe::Display::GamePad) / 480.0;
            logicalPoint = renderer_.outputToLogical(cafe::Display::GamePad, outputX, outputY);
        }
        if (rawTouching && !rawWasTouching_) touchStartedInside_ = logicalPoint.has_value();
        const bool touchActive = rawTouching && touchStartedInside_ && logicalPoint.has_value();
        if (touchActive) {
            lastTouchX_ = logicalPoint->x;
            lastTouchY_ = logicalPoint->y;
            input.pointerDisplay = cafe::Display::GamePad;
            input.mouse.x = lastTouchX_;
            input.mouse.y = lastTouchY_;
            input.mouse.left.down = true;
            input.mouse.left.pressed = !wasTouchActive_;
            input.mouse.left.released = false;
        } else {
            input.mouse.left.down = false;
            input.mouse.left.pressed = false;
            input.mouse.left.released = wasTouchActive_;
            if (!rawTouching && wasTouchActive_)
                engine.click(lastTouchX_, lastTouchY_, cafe::Display::GamePad);
        }
        if (!rawTouching) touchStartedInside_ = false;
        rawWasTouching_ = rawTouching;
        wasTouchActive_ = touchActive;
    }

    void pollWiiRemote(cafe::InputSystem& input) {
        cafe::WiiRemoteState next{};
        bool present = false;
        for (int chan = 0; chan < 4; ++chan) {
            KPADStatus status{};
            KPADError error = KPAD_ERROR_OK;
            const std::uint32_t read = KPADReadEx(static_cast<KPADChan>(chan), &status, 1, &error);
            if (error == KPAD_ERROR_NO_SAMPLES) { present = true; continue; }
            if (read == 0 || error != KPAD_ERROR_OK || status.error < 0) continue;

            present = true;
            next.connected = true;
            setButton(next.A, status.hold, status.trigger, status.release, WPAD_BUTTON_A);
            setButton(next.B, status.hold, status.trigger, status.release, WPAD_BUTTON_B);
            setButton(next.one, status.hold, status.trigger, status.release, WPAD_BUTTON_1);
            setButton(next.two, status.hold, status.trigger, status.release, WPAD_BUTTON_2);
            setButton(next.plus, status.hold, status.trigger, status.release, WPAD_BUTTON_PLUS);
            setButton(next.minus, status.hold, status.trigger, status.release, WPAD_BUTTON_MINUS);
            setButton(next.home, status.hold, status.trigger, status.release, WPAD_BUTTON_HOME);
            setButton(next.up, status.hold, status.trigger, status.release, WPAD_BUTTON_UP);
            setButton(next.down, status.hold, status.trigger, status.release, WPAD_BUTTON_DOWN);
            setButton(next.left, status.hold, status.trigger, status.release, WPAD_BUTTON_LEFT);
            setButton(next.right, status.hold, status.trigger, status.release, WPAD_BUTTON_RIGHT);
            next.accelerometer.x = status.acc.x;
            next.accelerometer.y = status.acc.y;
            next.accelerometer.z = status.acc.z;

            if (status.posValid) {
                const double outputX=std::clamp(static_cast<double>(status.pos.x)*0.5+0.5,0.0,1.0)*
                    renderer_.outputWidth(cafe::Display::Tv);
                const double outputY=std::clamp(static_cast<double>(status.pos.y)*0.5+0.5,0.0,1.0)*
                    renderer_.outputHeight(cafe::Display::Tv);
                if (const auto point=renderer_.outputToLogical(cafe::Display::Tv,
                                                                outputX,outputY)) {
                    next.pointerValid=true;
                    next.x=point->x;
                    next.y=point->y;
                }
            }

            if (status.extensionType == WPAD_EXT_NUNCHUK || status.extensionType == WPAD_EXT_MPLUS_NUNCHUK) {
                next.nunchuk.stick.x = status.nunchuk.stick.x;
                next.nunchuk.stick.y = status.nunchuk.stick.y;
                setButton(next.nunchuk.C, status.nunchuk.hold, status.nunchuk.trigger,
                          status.nunchuk.release, WPAD_NUNCHUK_BUTTON_C);
                setButton(next.nunchuk.Z, status.nunchuk.hold, status.nunchuk.trigger,
                          status.nunchuk.release, WPAD_NUNCHUK_BUTTON_Z);
            }
            input.wiiRemote = next;
            return;
        }
        if (!present) input.wiiRemote = {};
    }
};

struct SdCardMount {
    bool mounted{false};
    ~SdCardMount() {
        if (mounted) WHBUnmountSdCard();
    }
};

void recordBootStage(SdCardMount& sd,const char* stage) {
    static const auto bootStarted=std::chrono::steady_clock::now();
    static unsigned sequence=0;
    static bool detailedTrace=false;
    if (!sd.mounted && WHBMountSdCard()) sd.mounted=true;
    if (!sd.mounted) return;
    const char* root=WHBGetSdCardMountPath();
    if (!root) return;
    const auto path=std::filesystem::path(root)/"wiiu"/"apps"/
                    "cafe-engine"/"saves"/"boot-stage.txt";
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(),error);
    if (error) return;
    if (sequence==0 && std::string_view(stage)=="started") {
        detailedTrace=std::filesystem::exists(
            path.parent_path()/"boot-trace.enabled");
        if (!detailedTrace) {
            std::ofstream disabled(path.parent_path()/"boot-trace.txt",
                                   std::ios::trunc);
            if (disabled) disabled << "Detailed boot trace disabled.\n";
        }
    }
    std::ofstream output(path,std::ios::trunc);
    if (output) output << stage << '\n';
    if (!detailedTrace) return;
    const auto tracePath=path.parent_path()/"boot-trace.txt";
    std::ofstream trace(tracePath,std::ios::out |
        (sequence==0?std::ios::trunc:std::ios::app));
    if (trace) {
        const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-bootStarted).count();
        trace << sequence++ << ' ' << elapsed << "ms " << stage << '\n';
    }
}

std::filesystem::path storageRoot(SdCardMount& sd,
                                  const std::filesystem::path& project) {
    if (!sd.mounted && WHBMountSdCard()) sd.mounted = true;
    if (sd.mounted) {
        if (const char* root = WHBGetSdCardMountPath())
            return std::filesystem::path(root) / "wiiu" / "apps" /
                   "cafe-engine" / "saves";
    }
    // Never fall back to /vol/storage_* or another internal Cafe OS volume.
    // An empty root makes Storage writes fail safely until an SD is present.
    (void)project;
    return {};
}

std::filesystem::path findProject(SdCardMount& sd) {
    const char* bundled[] = {
        "/vol/content/main.uml",
        "fs:/vol/content/main.uml",
        "./main.uml",
        "main.uml",
    };
    for (const char* path : bundled) {
        if (cafe::fileReadable(path)) return path;
    }

    if (!WHBMountSdCard()) {
        throw std::runtime_error("No /vol/content/main.uml and the SD card would not mount");
    }
    sd.mounted = true;
    const char* root = WHBGetSdCardMountPath();
    if (!root) throw std::runtime_error("WUT did not provide an SD mount path");

    auto scan = [](const std::filesystem::path& dir, std::filesystem::path& found) {
        const auto direct = dir / "main.uml";
        if (cafe::fileReadable(direct)) {
            found = direct;
            return true;
        }
        DIR* handle = opendir(dir.string().c_str());
        if (!handle) return false;
        bool ok = false;
        while (dirent* entry = readdir(handle)) {
            if (!entry->d_name[0] || entry->d_name[0] == '.') continue;
            const auto path = dir / entry->d_name / "main.uml";
            if (cafe::fileReadable(path)) {
                found = path;
                ok = true;
                break;
            }
        }
        closedir(handle);
        return ok;
    };

    std::filesystem::path found;
    const std::filesystem::path sdRoot(root);
    if (scan(sdRoot, found)) return found;
    if (scan(sdRoot / "wiiu" / "apps", found)) return found;
    if (scan(sdRoot / "apps", found)) return found;
    throw std::runtime_error("No Cafe project. In Cemu: File > Load the .wuhb, or set the SD card to dist/");
}

void presentMessage(cafe::Renderer& renderer, const std::string& title, const std::string& body) {
    auto draw = [&](cafe::Display display) {
        renderer.beginDisplay(display, {36, 16, 28, 255});
        cafe::ComputedStyle style;
        style.textColor = {255, 236, 214, 255};
        cafe::Node heading;
        heading.text = title;
        heading.transform.position = {36, 150};
        style.fontSize = 28;
        renderer.drawText(heading, style);
        cafe::Node line;
        line.text = body;
        line.transform.position = {36, 210};
        style.fontSize = 16;
        renderer.drawText(line, style);
        renderer.endDisplay(display);
    };
    draw(cafe::Display::Tv);
    draw(cafe::Display::GamePad);
    renderer.present();
}

} // namespace

int main(int, char**) {
    WHBProcInit();
    int result = 0;
    SdCardMount sd;
    recordBootStage(sd,"started");
    try {
        auto renderer = std::make_unique<cafe::WiiURenderer>(640, 480);
        recordBootStage(sd,"graphics-ready");
        cafe::WiiURenderer* wiiURenderer = renderer.get();
        cafe::Engine engine(std::move(renderer));
        std::string error;
        bool loaded = false;
        try {
            const auto project = findProject(sd);
            OSReport("Cafe Engine project: %s\n", project.string().c_str());
            engine.setStorageRoot(storageRoot(sd, project));
            engine.loadProject(project);
            loaded = true;
            recordBootStage(sd,"project-loaded");
        } catch (const std::exception& errorValue) {
            error = errorValue.what();
            OSReport("Cafe Engine load: %s\n", error.c_str());
            recordBootStage(sd,"project-load-error");
        }

        if (loaded) {
            if (std::filesystem::exists(storageRoot(sd,{})/"boot-trace.enabled")) {
                const auto trace=[&sd](const char* stage) { recordBootStage(sd,stage); };
                wiiURenderer->setStartupTrace(trace);
                engine.setStartupTrace(trace);
            }
        }
        WiiUInputAdapter input(*wiiURenderer);
        recordBootStage(sd,"input-ready");
        bool firstFrame=true;
        bool renderErrorRecorded=false;
        auto previousFrame = std::chrono::steady_clock::now();
        constexpr double renderInterval = 1.0 / 60.0;
        double renderAccumulator = renderInterval;
#ifdef CAFE_RENDER_PROFILE
        cafe::WiiURenderer::FrameStats profileSum{};
        unsigned profileCount=0;
        double profileUpdateMs=0;
        double profileMaxUpdateMs=0,profileMaxRenderMs=0;
        cafe::Engine::UpdateStats profileUpdateParts{};
#endif
        while (WHBProcIsRunning()) {
            if (firstFrame) recordBootStage(sd,"loop-start");
            const auto now = std::chrono::steady_clock::now();
            const double deltaSeconds = std::clamp(
                std::chrono::duration<double>(now - previousFrame).count(),
                0.0, 0.25);
            previousFrame = now;
            if (loaded) {
                if (firstFrame) recordBootStage(sd,"layout-start");
                if (engine.refreshDisplayLayout())
                    renderAccumulator=renderInterval;
                if (firstFrame) recordBootStage(sd,"input-start");
                input.poll(engine);
                if (firstFrame) recordBootStage(sd,"input-done");
#ifdef CAFE_RENDER_PROFILE
                const auto updateStart=std::chrono::steady_clock::now();
#endif
                if (firstFrame) recordBootStage(sd,"update-start");
                engine.update(deltaSeconds);
                if (firstFrame) recordBootStage(sd,"update-done");
#ifdef CAFE_RENDER_PROFILE
                const auto updateParts=engine.lastUpdateStats();
                profileUpdateParts.hoverMs+=updateParts.hoverMs;
                profileUpdateParts.navigationMs+=updateParts.navigationMs;
                profileUpdateParts.scriptMs+=updateParts.scriptMs;
                profileUpdateParts.particlesMs+=updateParts.particlesMs;
                const double updateMs=std::chrono::duration<double,std::milli>(
                    std::chrono::steady_clock::now()-updateStart).count();
                profileUpdateMs+=updateMs;
                profileMaxUpdateMs=std::max(profileMaxUpdateMs,updateMs);
#endif
                renderAccumulator += deltaSeconds;
                if (renderAccumulator < renderInterval) {
                    // Do not burn an emulated PowerPC core while waiting for
                    // the 60 Hz frame boundary. Leaving a small margin avoids
                    // sleeping past it on real hardware.
                    const double remaining = renderInterval - renderAccumulator;
                    if (remaining > 0.0015) {
                        OSSleepTicks(OSMicrosecondsToTicks(static_cast<std::uint64_t>(
                            (remaining - 0.001) * 1000000.0)));
                    }
                    continue;
                }
                renderAccumulator = std::fmod(renderAccumulator, renderInterval);
                try {
                    if (firstFrame) recordBootStage(sd,"render-start");
                    engine.render();
                    if (firstFrame) {
                        wiiURenderer->setStartupTrace({});
                        engine.setStartupTrace({});
                        recordBootStage(sd,"first-frame");
                        firstFrame=false;
                    }
#ifdef CAFE_RENDER_PROFILE
                    const auto& stats=wiiURenderer->lastFrameStats();
                    profileSum.cpuFrameMs+=stats.cpuFrameMs;
                    profileMaxRenderMs=std::max(profileMaxRenderMs,stats.cpuFrameMs);
                    profileSum.rasterMs+=stats.rasterMs;
                    profileSum.geometryMs+=stats.geometryMs;
                    profileSum.uploadMs+=stats.uploadMs;
                    profileSum.presentMs+=stats.presentMs;
                    profileSum.fps+=stats.fps;
                    profileSum.drawCalls+=stats.drawCalls;
                    profileSum.textureUploads+=stats.textureUploads;
                    profileSum.uploadedBytes+=stats.uploadedBytes;
                    profileSum.sprites+=stats.sprites;
                    profileSum.particles+=stats.particles;
                    if (++profileCount==120) {
                        OSReport("Cafe GX2 avg/120: fps %.1f cpu %.2fms raster %.2fms geometry %.2fms upload %.2fms present %.2fms calls %u textures %u bytes %u sprites %u particles %u\n",
                            profileSum.fps/profileCount,profileSum.cpuFrameMs/profileCount,
                            profileSum.rasterMs/profileCount,profileSum.geometryMs/profileCount,
                            profileSum.uploadMs/profileCount,profileSum.presentMs/profileCount,
                            static_cast<unsigned>(profileSum.drawCalls/profileCount),
                            static_cast<unsigned>(profileSum.textureUploads/profileCount),
                            static_cast<unsigned>(profileSum.uploadedBytes/profileCount),
                            static_cast<unsigned>(profileSum.sprites/profileCount),
                            static_cast<unsigned>(profileSum.particles/profileCount));
                        if (sd.mounted) if (const char* root=WHBGetSdCardMountPath()) {
                            const auto path=std::filesystem::path(root)/"wiiu"/"apps"/
                                            "cafe-engine"/"saves"/"render-profile.csv";
                            const bool hadFile=std::filesystem::exists(path);
                            std::ofstream output(path,std::ios::app);
                            if (output) {
                                if (!hadFile)
                                    output << "fps,update_ms,render_cpu_ms,raster_ms,geometry_ms,upload_ms,present_ms,draw_calls,texture_uploads,uploaded_bytes,sprites,particles,hover_ms,navigation_ms,script_ms,particles_update_ms,max_update_ms,max_render_ms\n";
                                output << profileSum.fps/profileCount << ','
                                       << profileUpdateMs/profileCount << ','
                                       << profileSum.cpuFrameMs/profileCount << ','
                                       << profileSum.rasterMs/profileCount << ','
                                       << profileSum.geometryMs/profileCount << ','
                                       << profileSum.uploadMs/profileCount << ','
                                       << profileSum.presentMs/profileCount << ','
                                       << profileSum.drawCalls/profileCount << ','
                                       << profileSum.textureUploads/profileCount << ','
                                       << profileSum.uploadedBytes/profileCount << ','
                                       << profileSum.sprites/profileCount << ','
                                       << profileSum.particles/profileCount << ','
                                       << profileUpdateParts.hoverMs/profileCount << ','
                                       << profileUpdateParts.navigationMs/profileCount << ','
                                       << profileUpdateParts.scriptMs/profileCount << ','
                                       << profileUpdateParts.particlesMs/profileCount << ','
                                       << profileMaxUpdateMs << ',' << profileMaxRenderMs << '\n';
                            }
                        }
                        profileSum={}; profileCount=0; profileUpdateMs=0;
                        profileMaxUpdateMs=0;profileMaxRenderMs=0;
                        profileUpdateParts={};
                    }
#endif
                } catch (const std::exception& renderError) {
                    OSReport("Cafe Engine Wii U render: %s\n", renderError.what());
                    if (!renderErrorRecorded) {
                        recordBootStage(sd,"render-error");
                        renderErrorRecorded=true;
                    }
                    presentMessage(engine.renderer(), "Render failed", renderError.what());
                }
            } else {
                presentMessage(engine.renderer(), "Cafe Engine",
                               error.empty() ? "No game found" : error);
            }
        }
    } catch (const std::exception& error) {
        OSReport("Cafe Engine Wii U: %s\n", error.what());
        recordBootStage(sd,"startup-error");
        result = 1;
    }

    WHBProcShutdown();
    return result;
}
