#include "cafe/audio/AudioSystem.hpp"

#include <SDL.h>

#include <algorithm>
#include <cmath>

namespace cafe {

AudioSystem::AudioSystem() {
    if ((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
        ownsAudioSubsystem_ = true;
    }
    SDL_AudioSpec wanted{};
    wanted.freq = outputRate_;
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    wanted.samples = 1024;
    wanted.callback = &AudioSystem::callback;
    wanted.userdata = this;
    SDL_AudioSpec actual{};
    device_ = SDL_OpenAudioDevice(nullptr, 0, &wanted, &actual,
                                  SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (device_) {
        outputRate_ = actual.freq;
        SDL_PauseAudioDevice(device_, 0);
    } else if (ownsAudioSubsystem_) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        ownsAudioSubsystem_ = false;
    }
}

AudioSystem::~AudioSystem() {
    if (device_) SDL_CloseAudioDevice(device_);
    if (ownsAudioSubsystem_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

int AudioSystem::play(const Asset& clip, bool loop, double volume) {
    if (!device_ || !clip.available || clip.type != AssetType::Audio ||
        clip.samples.empty() || clip.channels <= 0 || clip.sampleRate <= 0) return 0;
    SDL_LockAudioDevice(device_);
    const int id = nextVoiceId_++;
    voices_.push_back({id, &clip, 0.0, std::clamp(volume, 0.0, 2.0), 1.0, loop, false});
    SDL_UnlockAudioDevice(device_);
    return id;
}

void AudioSystem::pause(int voiceId) {
    if (!device_) return;
    SDL_LockAudioDevice(device_);
    for (Voice& voice : voices_) if (voice.id == voiceId) voice.paused = true;
    SDL_UnlockAudioDevice(device_);
}

void AudioSystem::resume(int voiceId) {
    if (!device_) return;
    SDL_LockAudioDevice(device_);
    for (Voice& voice : voices_) if (voice.id == voiceId) voice.paused = false;
    SDL_UnlockAudioDevice(device_);
}

void AudioSystem::setVolume(int voiceId, double volume) {
    if (!device_) return;
    SDL_LockAudioDevice(device_);
    for (Voice& voice : voices_) if (voice.id == voiceId)
        voice.volume = std::clamp(volume, 0.0, 2.0);
    SDL_UnlockAudioDevice(device_);
}

void AudioSystem::setPitch(int voiceId, double pitch) {
    if (!device_) return;
    SDL_LockAudioDevice(device_);
    for (Voice& voice : voices_) if (voice.id == voiceId)
        voice.pitch = std::clamp(pitch, 0.05, 4.0);
    SDL_UnlockAudioDevice(device_);
}

bool AudioSystem::isPlaying(int voiceId) const {
    if (!device_) return false;
    SDL_LockAudioDevice(device_);
    const bool playing = std::any_of(voices_.begin(), voices_.end(), [voiceId](const Voice& voice) {
        return voice.id == voiceId && !voice.paused;
    });
    SDL_UnlockAudioDevice(device_);
    return playing;
}

void AudioSystem::stop(int voiceId) {
    if (!device_) return;
    SDL_LockAudioDevice(device_);
    std::erase_if(voices_, [voiceId](const Voice& voice) { return voice.id == voiceId; });
    SDL_UnlockAudioDevice(device_);
}

void AudioSystem::stopAll() {
    if (!device_) return;
    SDL_LockAudioDevice(device_);
    voices_.clear();
    SDL_UnlockAudioDevice(device_);
}

void AudioSystem::callback(void* userdata, std::uint8_t* stream, int length) {
    auto* self = static_cast<AudioSystem*>(userdata);
    auto* output = reinterpret_cast<std::int16_t*>(stream);
    const int frames = length / static_cast<int>(sizeof(std::int16_t) * 2);
    std::fill(output, output + frames * 2, 0);
    if (self) self->mix(output, frames);
}

void AudioSystem::mix(std::int16_t* output, int frames) {
    for (Voice& voice : voices_) {
        if (voice.paused) continue;
        const Asset& clip = *voice.clip;
        const std::size_t sourceFrames = clip.samples.size() / static_cast<std::size_t>(clip.channels);
        const double step = static_cast<double>(clip.sampleRate) /
            std::max(1, outputRate_) * voice.pitch;
        for (int frame = 0; frame < frames; ++frame) {
            if (voice.frame >= sourceFrames) {
                if (!voice.loop) break;
                voice.frame = std::fmod(voice.frame, static_cast<double>(sourceFrames));
            }
            const std::size_t sourceFrame = std::min(sourceFrames - 1,
                static_cast<std::size_t>(voice.frame));
            const std::size_t base = sourceFrame * static_cast<std::size_t>(clip.channels);
            const int left = clip.samples[base];
            const int right = clip.channels > 1 ? clip.samples[base + 1] : left;
            const int outputIndex = frame * 2;
            output[outputIndex] = static_cast<std::int16_t>(std::clamp(
                static_cast<int>(output[outputIndex] + left * voice.volume), -32768, 32767));
            output[outputIndex + 1] = static_cast<std::int16_t>(std::clamp(
                static_cast<int>(output[outputIndex + 1] + right * voice.volume), -32768, 32767));
            voice.frame += step;
        }
    }
    std::erase_if(voices_, [](const Voice& voice) {
        if (voice.loop || !voice.clip || voice.clip->channels <= 0) return false;
        return voice.frame >= voice.clip->samples.size() /
            static_cast<double>(voice.clip->channels);
    });
}

} // namespace cafe
