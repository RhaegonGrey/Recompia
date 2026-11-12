// SPDX-License-Identifier: GPL-3.0-or-later
// The Linux audio sink and output-device list: SDL3's audio subsystem, the counterpart of services/audio_xaudio2.cpp (WASAPI) and
// services/audio_coreaudio.cpp (AudioUnit). SDL3 reaches ALSA, PipeWire and PulseAudio through dlopen() at run time, so the build
// needs no audio headers and the package no extra library; it is the SDL3 the graphics shell already links (cmake/gfx.cmake).
//
// The sink is a device-bound audio stream: the guest's interleaved int16 frames go in with SDL_PutAudioStreamData and SDL converts
// them to whatever the device prefers (usually float32). SDL_GetAudioStreamQueued counts the input bytes still unplayed, which is a
// real device clock, so has_device_clock() is true and the audio renderer paces itself by the queue (about 40 ms) instead of by the
// host clock.
//
// [audio] device picks an endpoint by the id list_output_devices() reports. SDL's device ids only last for one run (they are
// assigned in enumeration order), so the id here is the device's name, and the sink accepts either the name or the numeric id.
// Without a usable device the stream opens on SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, which follows the system default.
// Without SDL3 (a build with the graphics shell off) there is no Linux audio: the stub at the end of this file returns no sink and
// no devices, and sink() falls back to the null sink.
#if defined(NXRT_HAVE_SDL3_AUDIO)
#include <SDL3/SDL.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/text.h"

namespace nxrt::audio {
namespace {

constexpr const char* kTag = "audio";

// The queue cap: 250 ms. The renderer keeps about 40 ms queued, so the cap only matters when the device stops draining (a suspended
// session, a device that vanished mid-frame): past it the frames are dropped instead of growing a buffer without bound.
constexpr uint32_t kMaxQueuedMs = 250;

// Device names are UTF-8; wchar_t is UTF-32 on Linux, so decode the code points directly (same helper as audio_devices_posix.cpp,
// which is the macOS-only list).
std::wstring widen(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char b = static_cast<unsigned char>(s[i]);
        uint32_t cp = b;
        size_t n = 1;
        if (b >= 0xF0) { cp = b & 0x07; n = 4; }
        else if (b >= 0xE0) { cp = b & 0x0F; n = 3; }
        else if (b >= 0xC0) { cp = b & 0x1F; n = 2; }
        for (size_t k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        i += n;
        out.push_back(static_cast<wchar_t>(cp <= 0x10FFFF ? cp : 0xFFFD));
    }
    return out;
}

// The playback device named in the settings: by name (case and surrounding blanks ignored) or by the numeric SDL id. 0 when the
// name matches nothing, which sends the caller to the default device.
SDL_AudioDeviceID device_by_name(const std::string& want) {
    int n = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n);
    if (!ids) return 0;
    SDL_AudioDeviceID found = 0;
    const std::string key = text::lower_trim(want);
    for (int i = 0; i < n && !found; ++i) {
        const char* name = SDL_GetAudioDeviceName(ids[i]);
        if (!name) continue;
        if (text::lower_trim(name) == key || std::to_string(ids[i]) == want) found = ids[i];
    }
    SDL_free(ids);
    return found;
}

class Sdl3Sink final : public AudioSink {
public:
    Sdl3Sink() = default;
    ~Sdl3Sink() override { close(); }
    Sdl3Sink(const Sdl3Sink&) = delete;
    Sdl3Sink& operator=(const Sdl3Sink&) = delete;

    bool open(uint32_t sample_rate, uint32_t channels) override {
        std::lock_guard<std::mutex> lk(mu_);
        close_locked();
        rate_ = sample_rate ? sample_rate : 48000;
        channels_ = channels ? channels : 2;
        stats_ = SinkStats{};
        stats_.sample_rate = rate_;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            NXLOG_W(kTag, "SDL3 audio does not initialize: %s", SDL_GetError());
            return false;
        }
        const std::string want = config_get_str("audio", "device", "");
        if (!want.empty()) {
            const SDL_AudioDeviceID named = device_by_name(want);
            if (named && start_stream(named)) return true;
            if (named) NXLOG_W(kTag, "SDL3: device '%s' does not open, using the default device", want.c_str());
            else NXLOG_W(kTag, "SDL3: no output device is named '%s', using the default device", want.c_str());
        }
        if (start_stream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK)) return true;
        NXLOG_W(kTag, "SDL3: no output device could be opened: %s", SDL_GetError());
        return false;
    }

    void close() override {
        std::lock_guard<std::mutex> lk(mu_);
        close_locked();
    }

    uint32_t write(const int16_t* pcm, uint32_t frames) override {
        if (!pcm || !frames) return 0;
        std::lock_guard<std::mutex> lk(mu_);
        stats_.frames_written += frames;
        if (!stream_ || !started_) {
            stats_.frames_dropped += frames;
            return 0;
        }
        const uint32_t max_frames = rate_ / 1000 * kMaxQueuedMs;
        const uint32_t held = queued_locked();
        const uint32_t take = std::min(frames, max_frames > held ? max_frames - held : 0);
        if (take) {
            // The player may have changed the master volume or muted since the stream opened: the gain is the sink's volume.
            const float gain = effective_volume();
            if (gain != gain_) {
                gain_ = gain;
                SDL_SetAudioStreamGain(stream_, gain_);
            }
            const int bytes = static_cast<int>(take * channels_ * sizeof(int16_t));
            if (!SDL_PutAudioStreamData(stream_, pcm, bytes)) {
                NXLOG_W(kTag, "SDL3: the stream refuses data: %s", SDL_GetError());
                stats_.frames_dropped += frames;
                return 0;
            }
        }
        stats_.frames_accepted += take;
        stats_.frames_dropped += frames - take;
        const uint32_t after = queued_locked();
        stats_.queued_peak_frames = std::max(stats_.queued_peak_frames, after);
        if (started_ && after && (!stats_.queued_low_frames || after < stats_.queued_low_frames)) stats_.queued_low_frames = after;
        return take;
    }

    uint32_t queued_frames() override {
        std::lock_guard<std::mutex> lk(mu_);
        if (!stream_ || !started_) return 0;
        const uint32_t held = queued_locked();
        // An underrun is the device running dry after playback started: count the transition, not every empty poll.
        if (held == 0 && !empty_) ++stats_.underruns;
        empty_ = held == 0;
        if (held && (!stats_.queued_low_frames || held < stats_.queued_low_frames)) stats_.queued_low_frames = held;
        return held;
    }

    bool has_device_clock() const override { return true; }
    const char* name() const override { return "sdl3"; }

    SinkStats stats() override {
        std::lock_guard<std::mutex> lk(mu_);
        SinkStats s = stats_;
        s.queued_peak_frames = std::max(s.queued_peak_frames, queued_locked());
        return s;
    }

private:
    // Bytes still queued, in frames. SDL counts the bytes put in (the input format, int16 at channels_), not the device format.
    uint32_t queued_locked() const {
        if (!stream_) return 0;
        const int bytes = SDL_GetAudioStreamQueued(stream_);
        if (bytes <= 0) return 0;
        return static_cast<uint32_t>(bytes) / (channels_ * sizeof(int16_t));
    }

    bool start_stream(SDL_AudioDeviceID dev) {
        const SDL_AudioSpec spec{SDL_AUDIO_S16LE, static_cast<int>(channels_), static_cast<int>(rate_)};
        stream_ = SDL_OpenAudioDeviceStream(dev, &spec, nullptr, nullptr);
        if (!stream_) return false;
        gain_ = effective_volume();
        SDL_SetAudioStreamGain(stream_, gain_);
        if (!SDL_ResumeAudioStreamDevice(stream_)) {
            SDL_DestroyAudioStream(stream_);
            stream_ = nullptr;
            return false;
        }
        started_ = true;
        empty_ = true;
        const char* dev_name = dev == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ? nullptr : SDL_GetAudioDeviceName(dev);
        NXLOG_I(kTag, "SDL3: %u Hz, %u channels, %s%s", rate_, channels_, dev_name ? "device " : "default device",
                dev_name ? dev_name : "");
        return true;
    }

    void close_locked() {
        if (stream_) {
            SDL_DestroyAudioStream(stream_);
            stream_ = nullptr;
        }
        if (started_) NXLOG_I(kTag, "%s", format_stats(name(), stats_).c_str());
        started_ = false;
        empty_ = true;
    }

    std::mutex mu_;
    SDL_AudioStream* stream_ = nullptr;
    uint32_t rate_ = 0;
    uint32_t channels_ = 0;
    float gain_ = 1.0f;
    bool started_ = false;
    bool empty_ = true;
    SinkStats stats_{};
};

}  // namespace

std::shared_ptr<AudioSink> make_sdl3_sink() { return std::make_shared<Sdl3Sink>(); }

std::vector<OutputDevice> list_output_devices() {
    std::vector<OutputDevice> out;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        NXLOG_W(kTag, "SDL3 audio does not initialize, no output devices to list: %s", SDL_GetError());
        return out;
    }
    int n = 0;
    SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n);
    if (!ids) return out;
    // The default device is the one a stream on SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK lands on; SDL has no query for it, and the name
    // it reports for that logical id is the name of the device, so the comparison is by name.
    const char* def = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
    const std::string def_name = def ? def : "";
    for (int i = 0; i < n; ++i) {
        const char* name = SDL_GetAudioDeviceName(ids[i]);
        OutputDevice d;
        d.name = name && *name ? name : "Audio device";
        d.id = widen(d.name);   // stable across runs, unlike the SDL id
        d.is_default = !def_name.empty() && def_name == d.name;
        out.push_back(std::move(d));
    }
    SDL_free(ids);
    return out;
}

}  // namespace nxrt::audio

#else   // NXRT_HAVE_SDL3_AUDIO

#include "nxrt/audio_sink.h"

#include <string>
#include <vector>

namespace nxrt::audio {

std::shared_ptr<AudioSink> make_sdl3_sink() { return nullptr; }
std::vector<OutputDevice> list_output_devices() { return {}; }

}  // namespace nxrt::audio

#endif   // NXRT_HAVE_SDL3_AUDIO
