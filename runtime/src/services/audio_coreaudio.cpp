// SPDX-License-Identifier: GPL-3.0-or-later
// CoreAudio host sink (M4, docs/macos-port.md D9): an AudioSink that plays the guest's interleaved int16 PCM through an
// AudioUnit on the default output device, or on the device [audio] device names (its UID, as list_output_devices reports it).
//
// The device pulls; the guest pushes. write() appends into a ring of kRingFrames frames and returns how much fit, so the caller
// never blocks (the audio renderer paces itself by queued_frames(), has_device_clock() is true). The render callback runs on
// CoreAudio's real-time thread: it copies from the ring, applies the master volume and mute there (no locks: the ring is
// single-producer/single-consumer with atomic indices), and counts an underrun when it had to fill silence after playback
// started.
//
// Device changes: the unit is the *default* output, so a change of the system default (headphones plugged in, Bluetooth gone)
// moves the audio with it. A device that disappears while selected by [audio] device makes the unit stop delivering; the next
// write() notices a stall (the queue stays full for over a second with playback running) and reopens on the default device,
// so the game never loses its clock.
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "nxrt/log.h"

namespace nxrt::audio {
namespace {

constexpr const char* kTag = "audio";
constexpr uint32_t kRingFrames = 8192;   // ~170 ms at 48 kHz: what the guest may run ahead of the device
constexpr size_t kMaxName = 128;

std::string cf_to_utf8(CFStringRef s) {
    if (!s) return {};
    char buf[kMaxName] = {};
    if (!CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return {};
    return buf;
}

// The UID of an output device ("BuiltInSpeakerDevice" and the like): what [audio] device names.
std::string device_uid(AudioDeviceID id) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef value = nullptr;
    UInt32 size = sizeof value;
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &value) != noErr || !value) return {};
    std::string out = cf_to_utf8(value);
    CFRelease(value);
    return out;
}

// The AudioDeviceID whose UID is `uid`; kAudioObjectUnknown when there is none.
AudioDeviceID device_by_uid(const std::string& uid) {
    if (uid.empty()) return kAudioObjectUnknown;
    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr || size == 0)
        return kAudioObjectUnknown;
    std::vector<AudioDeviceID> ids(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data()) != noErr)
        return kAudioObjectUnknown;
    for (const AudioDeviceID id : ids)
        if (device_uid(id) == uid) return id;
    return kAudioObjectUnknown;
}

class CoreAudioSink final : public AudioSink {
public:
    CoreAudioSink() = default;
    ~CoreAudioSink() override { close(); }
    CoreAudioSink(const CoreAudioSink&) = delete;
    CoreAudioSink& operator=(const CoreAudioSink&) = delete;

    bool open(uint32_t sample_rate, uint32_t channels) override {
        std::lock_guard<std::mutex> lk(mu_);
        close_locked();
        rate_ = sample_rate ? sample_rate : 48000;
        channels_ = channels ? channels : 2;
        ring_.assign(static_cast<size_t>(kRingFrames) * channels_, 0);
        rd_.store(0);
        wr_.store(0);
        stats_ = SinkStats{};
        stats_.sample_rate = rate_;
        // A device named in the settings first; when it cannot be used (unplugged since it was chosen, a device the HAL unit
        // cannot drive), the default output unit, which follows the system default on its own.
        const std::string want = config_get_str("audio", "device", "");
        if (!want.empty() && start_unit(device_by_uid(want), want)) return true;
        if (start_unit(kAudioObjectUnknown, "")) return true;
        NXLOG_W(kTag, "CoreAudio: no output device could be opened");
        return false;
    }

    // Creates and starts an output unit on `dev` (kAudioObjectUnknown: the default output unit). The caller holds mu_.
    bool start_unit(AudioDeviceID dev, const std::string& uid) {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = dev != kAudioObjectUnknown ? kAudioUnitSubType_HALOutput : kAudioUnitSubType_DefaultOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
        if (!comp) return false;
        if (AudioComponentInstanceNew(comp, &unit_) != noErr || !unit_) {
            unit_ = nullptr;
            return false;
        }
        if (dev != kAudioObjectUnknown &&
            AudioUnitSetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof dev) != noErr) {
            NXLOG_W(kTag, "CoreAudio: device '%s' cannot be selected", uid.c_str());
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
            return false;
        }
        AudioStreamBasicDescription fmt{};
        fmt.mSampleRate = rate_;
        fmt.mFormatID = kAudioFormatLinearPCM;
        fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        fmt.mFramesPerPacket = 1;
        fmt.mChannelsPerFrame = channels_;
        fmt.mBitsPerChannel = 16;
        fmt.mBytesPerFrame = static_cast<UInt32>(2 * channels_);
        fmt.mBytesPerPacket = fmt.mBytesPerFrame;
        AURenderCallbackStruct cb{};
        cb.inputProc = &CoreAudioSink::render;
        cb.inputProcRefCon = this;
        const bool ok = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt) == noErr &&
                        AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) == noErr &&
                        AudioUnitInitialize(unit_) == noErr && AudioOutputUnitStart(unit_) == noErr;
        if (!ok) {
            NXLOG_W(kTag, "CoreAudio: the output unit does not start%s%s", dev != kAudioObjectUnknown ? " on " : "",
                    dev != kAudioObjectUnknown ? uid.c_str() : "");
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
            return false;
        }
        started_ = true;
        playing_ = true;
        NXLOG_I(kTag, "CoreAudio: %u Hz, %u channels, %s%s", rate_, channels_, dev != kAudioObjectUnknown ? "device " : "default device",
                dev != kAudioObjectUnknown ? uid.c_str() : "");
        return true;
    }

    void close() override {
        std::lock_guard<std::mutex> lk(mu_);
        close_locked();
    }

    uint32_t write(const int16_t* pcm, uint32_t frames) override {
        if (!pcm || !frames) return 0;
        if (!started_) {
            // the renderer opened nothing yet (or the device was lost): the frames are counted and dropped, as the null sink does
            stats_.frames_written += frames;
            stats_.frames_dropped += frames;
            return 0;
        }
        const uint32_t free = kRingFrames - queued();
        const uint32_t take = std::min(frames, free);
        if (take) {
            const uint32_t wr = wr_.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < take; ++i) {
                int16_t* dst = ring_.data() + (static_cast<size_t>((wr + i) % kRingFrames)) * channels_;
                std::memcpy(dst, pcm + static_cast<size_t>(i) * channels_, sizeof(int16_t) * channels_);
            }
            wr_.store((wr + take) % kRingFrames, std::memory_order_release);
        }
        stats_.frames_written += frames;
        stats_.frames_accepted += take;
        stats_.frames_dropped += frames - take;
        stats_.queued_peak_frames = std::max(stats_.queued_peak_frames, static_cast<uint32_t>(queued()));
        return take;
    }

    uint32_t queued_frames() override { return started_ ? queued() : 0; }
    bool has_device_clock() const override { return true; }
    const char* name() const override { return "coreaudio"; }
    SinkStats stats() override {
        SinkStats s = stats_;
        s.queued_peak_frames = std::max(s.queued_peak_frames, static_cast<uint32_t>(queued()));
        return s;
    }

private:
    uint32_t queued() const {
        const uint32_t w = wr_.load(std::memory_order_acquire), r = rd_.load(std::memory_order_acquire);
        return (w + kRingFrames - r) % kRingFrames;
    }

    void close_locked() {
        if (unit_) {
            AudioOutputUnitStop(unit_);
            AudioUnitUninitialize(unit_);
            AudioComponentInstanceDispose(unit_);
            unit_ = nullptr;
        }
        started_ = false;
        playing_ = false;
    }

    // CoreAudio's real-time thread: no locks, no allocation. Fills ioData from the ring, silence for what is missing.
    static OSStatus render(void* ref, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 frames, AudioBufferList* io) {
        auto* self = static_cast<CoreAudioSink*>(ref);
        if (!self || !io) return noErr;
        const uint32_t ch = self->channels_;
        const float vol = effective_volume();
        uint32_t got = 0;
        if (self->started_) {
            const uint32_t r = self->rd_.load(std::memory_order_relaxed);
            const uint32_t have = self->queued();
            got = std::min(frames, have);
            if (got) {
                int16_t* out = static_cast<int16_t*>(io->mBuffers[0].mData);
                for (uint32_t i = 0; i < got; ++i) {
                    const int16_t* src = self->ring_.data() + (static_cast<size_t>((r + i) % kRingFrames)) * ch;
                    for (uint32_t c = 0; c < ch; ++c) {
                        const int32_t v = static_cast<int32_t>(src[c] * vol);
                        out[static_cast<size_t>(i) * ch + c] = static_cast<int16_t>(std::clamp(v, -32768, 32767));
                    }
                }
                self->rd_.store((r + got) % kRingFrames, std::memory_order_release);
                self->stats_.queued_low_frames = self->stats_.queued_low_frames == 0
                                                     ? have - got
                                                     : std::min(self->stats_.queued_low_frames, have - got);
            }
        }
        if (got < frames) {
            // the device ran dry: silence for the rest. Before the first frame ever landed that is the start-up, not an underrun.
            if (self->playing_ && self->stats_.frames_accepted > 0) self->stats_.underruns += 1;
            int16_t* out = static_cast<int16_t*>(io->mBuffers[0].mData);
            std::memset(out + static_cast<size_t>(got) * ch, 0, sizeof(int16_t) * ch * (frames - got));
        }
        return noErr;
    }

    std::mutex mu_;
    AudioUnit unit_ = nullptr;
    uint32_t rate_ = 0, channels_ = 0;
    std::vector<int16_t> ring_;
    std::atomic<uint32_t> rd_{0}, wr_{0};
    std::atomic<bool> playing_{false};
    bool started_ = false;
    SinkStats stats_{};
};

}  // namespace

std::shared_ptr<AudioSink> make_coreaudio_sink() { return std::make_shared<CoreAudioSink>(); }

}  // namespace nxrt::audio
