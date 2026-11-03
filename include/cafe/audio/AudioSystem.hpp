#pragma once

#include "cafe/assets/AssetManager.hpp"

#include <cstdint>
#include <vector>

namespace cafe {

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();
    AudioSystem(const AudioSystem&) = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    [[nodiscard]] bool ready() const { return device_ != 0; }
    int play(const Asset& clip, bool loop = false, double volume = 1.0);
    void stop(int voiceId);
    void pause(int voiceId);
    void resume(int voiceId);
    void setVolume(int voiceId, double volume);
    void setPitch(int voiceId, double pitch);
    [[nodiscard]] bool isPlaying(int voiceId) const;
    void stopAll();

private:
    struct Voice {
        int id{0};
        const Asset* clip{nullptr};
        double frame{0.0};
        double volume{1.0};
        double pitch{1.0};
        bool loop{false};
        bool paused{false};
    };

    std::uint32_t device_{0};
    int outputRate_{48000};
    int nextVoiceId_{1};
    bool ownsAudioSubsystem_{false};
    std::vector<Voice> voices_;

    static void callback(void* userdata, std::uint8_t* stream, int length);
    void mix(std::int16_t* output, int frames);
};

} // namespace cafe
