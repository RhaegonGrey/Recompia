// SPDX-License-Identifier: GPL-3.0-or-later
// Extra voices mixed under the game's own output (include/nxrt/audio_sink.h, extra_voice_*): a ring of PCM16 stereo per voice, and the sink
// wrapper that adds them to what the audio services write. The wrapper takes as many frames from each voice as the game writes, so the
// voices play at the game's own pace and stop with it (a paused output plays no voices).
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/log.h"

namespace nxrt::audio {
namespace {

constexpr uint32_t kVoiceRate = 48000;
constexpr uint32_t kRingFrames = kVoiceRate;   // one second a voice

struct Voice {
    std::vector<int16_t> ring = std::vector<int16_t>(kRingFrames * 2);
    uint32_t head = 0, count = 0;   // frames: where the oldest is, how many are queued
};

struct Voices {
    std::mutex mu;
    std::array<Voice, kExtraVoices> v;
    std::atomic<uint32_t> queued{0};   // frames queued over every voice: the mixer's cheap "nothing to add"
};
Voices& voices() {
    static Voices s;
    return s;
}

bool valid(int voice) { return voice >= 0 && voice < kExtraVoices; }

int16_t sat(int32_t x) { return static_cast<int16_t>(std::clamp(x, -32768, 32767)); }

// Adds up to `frames` frames of every voice into pcm (channels per frame: 2, or 6 with the voices in front left and right) and takes them
// off the voices.
void mix_into(int16_t* pcm, uint32_t frames, uint32_t channels) {
    Voices& s = voices();
    std::lock_guard<std::mutex> lk(s.mu);
    uint32_t taken = 0;
    for (Voice& v : s.v) {
        const uint32_t n = std::min(frames, v.count);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t at = (v.head + i) % kRingFrames;
            int16_t* f = pcm + static_cast<size_t>(i) * channels;
            f[0] = sat(f[0] + v.ring[at * 2]);
            f[1] = sat(f[1] + v.ring[at * 2 + 1]);
        }
        v.head = (v.head + n) % kRingFrames;
        v.count -= n;
        taken += n;
    }
    s.queued.fetch_sub(taken, std::memory_order_relaxed);
}

// The sink the audio services see: forwards everything to inner, with the voices added to each write while the output is at the voices'
// rate in stereo or 5.1.
class VoiceMixSink final : public AudioSink {
public:
    explicit VoiceMixSink(std::shared_ptr<AudioSink> inner) : inner_(std::move(inner)) {}
    bool open(uint32_t sample_rate, uint32_t channels) override {
        rate_ = sample_rate;
        channels_ = channels;
        if (sample_rate != kVoiceRate && !warned_) {
            warned_ = true;
            NXLOG_W("audio", "the extra voices play at %u Hz and the output is at %u Hz: they are left out", kVoiceRate, sample_rate);
        }
        return inner_->open(sample_rate, channels);
    }
    void close() override { inner_->close(); }
    uint32_t write(const int16_t* pcm, uint32_t frames) override {
        if (voices().queued.load(std::memory_order_relaxed) == 0 || rate_ != kVoiceRate || (channels_ != 2 && channels_ != 6))
            return inner_->write(pcm, frames);
        scratch_.assign(pcm, pcm + static_cast<size_t>(frames) * channels_);
        mix_into(scratch_.data(), frames, channels_);
        return inner_->write(scratch_.data(), frames);
    }
    uint32_t queued_frames() override { return inner_->queued_frames(); }
    bool has_device_clock() const override { return inner_->has_device_clock(); }
    const char* name() const override { return inner_->name(); }
    SinkStats stats() override { return inner_->stats(); }

private:
    std::shared_ptr<AudioSink> inner_;
    uint32_t rate_ = 0, channels_ = 0;
    bool warned_ = false;
    std::vector<int16_t> scratch_;   // write() runs on one thread at a time (the output's own)
};

}  // namespace

std::shared_ptr<AudioSink> make_voice_mixer(std::shared_ptr<AudioSink> inner) { return std::make_shared<VoiceMixSink>(std::move(inner)); }

uint32_t extra_voice_write(int voice, const int16_t* stereo, uint32_t frames) {
    if (!valid(voice) || !stereo) return 0;
    Voices& s = voices();
    std::lock_guard<std::mutex> lk(s.mu);
    Voice& v = s.v[static_cast<size_t>(voice)];
    const uint32_t n = std::min(frames, kRingFrames - v.count);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t at = (v.head + v.count + i) % kRingFrames;
        v.ring[at * 2] = stereo[i * 2];
        v.ring[at * 2 + 1] = stereo[i * 2 + 1];
    }
    v.count += n;
    s.queued.fetch_add(n, std::memory_order_relaxed);
    return n;
}

uint32_t extra_voice_queued(int voice) {
    if (!valid(voice)) return 0;
    Voices& s = voices();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.v[static_cast<size_t>(voice)].count;
}

void extra_voice_clear(int voice) {
    if (!valid(voice)) return;
    Voices& s = voices();
    std::lock_guard<std::mutex> lk(s.mu);
    Voice& v = s.v[static_cast<size_t>(voice)];
    s.queued.fetch_sub(v.count, std::memory_order_relaxed);
    v.head = v.count = 0;
}

}  // namespace nxrt::audio
