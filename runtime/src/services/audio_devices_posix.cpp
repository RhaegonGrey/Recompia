// SPDX-License-Identifier: GPL-3.0-or-later
// The output endpoints the settings page offers ([audio] device): CoreAudio's device list, the macOS counterpart of
// services/audio_devices.cpp (WASAPI). Only devices with an output stream are listed; the default output device is marked.
// Nothing is opened here: the sink does that (M4: the CoreAudio sink itself is still to come, docs/macos-port.md D9).
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <string>
#include <vector>

#include "nxrt/audio_sink.h"

namespace nxrt::audio {
namespace {

std::string cf_string(CFStringRef s) {
    if (!s) return {};
    char buf[512] = {};
    if (!CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return {};
    return buf;
}

// CoreAudio returns CFStrings; the caller owns them (GetRule: the property copy must be released).
std::string device_string(AudioDeviceID id, AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress addr{selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef value = nullptr;
    UInt32 size = sizeof value;
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, &value) != noErr || !value) return {};
    const std::string out = cf_string(value);
    CFRelease(value);
    return out;
}

// Frames of output the device can carry (the sum of its output streams' channels); 0 for an input-only device.
uint32_t output_channels(AudioDeviceID id) {
    AudioObjectPropertyAddress addr{kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(id, &addr, 0, nullptr, &size) != noErr || size == 0) return 0;
    std::vector<uint8_t> storage(size);
    if (AudioObjectGetPropertyData(id, &addr, 0, nullptr, &size, storage.data()) != noErr) return 0;
    const auto* list = reinterpret_cast<const AudioBufferList*>(storage.data());
    uint32_t channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) channels += list->mBuffers[i].mNumberChannels;
    return channels;
}

std::wstring widen(const std::string& s) {
    // Device names and UIDs are UTF-8; wchar_t is UTF-32 on Darwin, so decode the code points directly.
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

}  // namespace

std::vector<OutputDevice> list_output_devices() {
    std::vector<OutputDevice> out;

    AudioDeviceID default_id = kAudioObjectUnknown;
    AudioObjectPropertyAddress def{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                   kAudioObjectPropertyElementMain};
    UInt32 def_size = sizeof default_id;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &def, 0, nullptr, &def_size, &default_id) != noErr)
        default_id = kAudioObjectUnknown;

    AudioObjectPropertyAddress addr{kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr || size == 0) return out;
    std::vector<AudioDeviceID> ids(size / sizeof(AudioDeviceID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data()) != noErr) return out;

    for (const AudioDeviceID id : ids) {
        if (output_channels(id) == 0) continue;
        OutputDevice d;
        d.id = widen(device_string(id, kAudioDevicePropertyDeviceUID));
        d.name = device_string(id, kAudioObjectPropertyName);
        if (d.name.empty()) d.name = "Audio device";
        d.is_default = id == default_id;
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace nxrt::audio
