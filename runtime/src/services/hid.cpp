// SPDX-License-Identifier: GPL-3.0-or-later
// hid service (IHidServer, S3c): the applet resource, the shared memory the SDK reads controller state from, the sampling that
// fills it from nxrt::hid::input_source(), and the commands of nn::hid::IHidServer.
// Adapted from Ryujinx Ryujinx.HLE/HOS/Services/Hid (IHidServer.cs: command ids, argument order and results; HidDevices/NpadDevices.cs:
// pad setup and lifo writes; Hid.cs UpdateStickButtons: the stick threshold; HidServer/HidUtils.cs: NpadIdType mapping; Types/SharedMemory:
// layouts; MIT License) and from ScarletNX src/nx/hle/services/hid.swipc (the command table; GPL-3.0-or-later).
// Command semantics: Switchbrew "HID services" (https://switchbrew.org/wiki/HID_services).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "hid_shm.h"
#include "nxrt/gauge.h"
#include "nxrt/config.h"
#include "nxrt/input_source.h"
#include "nxrt/jc2_mouse.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "services_internal.h"
#include "hid_internal.h"

namespace nxrt::services {

namespace hidsvc {

// Never destroyed: the sampler thread is joined by hid_reset(), so no static destructor has to deal with it.
HidState& st() {
    static HidState* s = new HidState();
    return *s;
}

}  // namespace hidsvc

using namespace hidsvc;

namespace {

// ---- shared memory --------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kCommonLifos[] = {kFullKeyLifo, kHandheldLifo, kJoyDualLifo, kJoyLeftLifo, kJoyRightLifo, kPalmaLifo, kSystemExtLifo};
constexpr uint64_t kSixAxisLifos[] = {kFullKeySixAxisLifo, kHandheldSixAxisLifo, kJoyDualLeftSixAxisLifo, kJoyDualRightSixAxisLifo,
                                      kJoyLeftSixAxisLifo, kJoyRightSixAxisLifo};

// Ryujinx SharedMemory.Create() and NpadState.Create(): every ring gets its buffer count, the npad condition its start values.
void init_shm(uint8_t* host) {
    std::memset(host, 0, kSize);
    ring_init<DebugPadState>(host, kDebugPad);
    ring_init<TouchScreenState>(host, kTouchScreen);
    ring_init<MouseState>(host, kMouse);
    ring_init<KeyboardState>(host, kKeyboard);
    for (uint32_t i = 0; i < kNpadCount; ++i) {
        uint8_t* e = host + kNpads + i * kNpadEntrySize;
        for (uint64_t off : kCommonLifos) ring_init<NpadCommonState>(e, off);
        for (uint64_t off : kSixAxisLifos) ring_init<SixAxisState>(e, off);
    }
    const uint32_t cond[4] = {0, 1, 1, 1};   // NpadCondition: _00 = 0, _04 = 1, hold type Horizontal (1), _0C = 1
    std::memcpy(host + kNpadCondition, cond, sizeof cond);
}

// Creates the shared memory on first use, and again when the kernel region it lived in is gone (tests re-initialise the runtime).
// Caller holds st().mu.
bool shm_ready(HidState& s) {
    if (s.shm && s.shm->ok() && mem::is_mapped(s.shm->host(), kSize)) return true;
    s.shm.emplace(kSize, "hid");
    if (!s.shm->ok()) {
        s.shm.reset();
        return false;
    }
    init_shm(reinterpret_cast<uint8_t*>(s.shm->host()));
    return true;
}

// Always through host(): it moves to the guest's view once the guest maps the object. Caller holds st().mu.
uint8_t* host_of(HidState& s) {
    if (!shm_ready(s)) return nullptr;
    return reinterpret_cast<uint8_t*>(s.shm->host());
}

// ---- mapping of ids and styles ---------------------------------------------------------------------------------------------------------
// Shared-memory entry index of a NpadIdType (-1 when invalid).
int index_of_npad_id(uint32_t id) {
    if (id < 8) return static_cast<int>(id);
    if (id == kNpadIdHandheld) return 8;
    if (id == kNpadIdOther) return 9;
    return -1;
}
// The input slot (input_source.h) that feeds a shared-memory entry.
uint32_t slot_of_index(uint32_t idx) {
    if (idx < 8) return idx;
    return idx == 8 ? nxrt::hid::kNpadSlotHandheld : nxrt::hid::kNpadSlotOther;
}
uint32_t tag_of_style(NpadStyle s) {
    switch (s) {
        case NpadStyle::FullKey: return kTagFullKey;
        case NpadStyle::Handheld: return kTagHandheld;
        case NpadStyle::JoyDual: return kTagJoyDual;
        case NpadStyle::JoyLeft: return kTagJoyLeft;
        case NpadStyle::JoyRight: return kTagJoyRight;
        default: return 0;
    }
}
uint64_t common_lifo_of_tag(uint32_t tag) {
    switch (tag) {
        case kTagFullKey: return kFullKeyLifo;
        case kTagHandheld: return kHandheldLifo;
        case kTagJoyDual: return kJoyDualLifo;
        case kTagJoyLeft: return kJoyLeftLifo;
        case kTagJoyRight: return kJoyRightLifo;
        default: return 0;
    }
}
bool six_lifo_of_tag(uint32_t tag, uint64_t off) {
    switch (tag) {
        case kTagFullKey: return off == kFullKeySixAxisLifo;
        case kTagHandheld: return off == kHandheldSixAxisLifo;
        case kTagJoyDual: return off == kJoyDualLeftSixAxisLifo || off == kJoyDualRightSixAxisLifo;
        case kTagJoyLeft: return off == kJoyLeftSixAxisLifo;
        case kTagJoyRight: return off == kJoyRightSixAxisLifo;
        default: return false;
    }
}
uint32_t attributes_of_tag(uint32_t tag) {
    uint32_t a = kAttrConnected;
    switch (tag) {
        case kTagFullKey:
        case kTagHandheld: a |= kAttrWired; break;
        case kTagJoyDual: a |= kAttrLeftConnected | kAttrRightConnected; break;
        case kTagJoyLeft: a |= kAttrLeftConnected; break;
        case kTagJoyRight: a |= kAttrRightConnected; break;
        default: break;
    }
    return a;
}

// Ryujinx Hid.cs UpdateStickButtons: a stick past half its range sets the direction bit (Dy > threshold is Up).
uint64_t stick_buttons(const StickValue& l, const StickValue& r) {
    using namespace nxrt::hid;
    uint64_t b = 0;
    if (l.x < -kStickButtonThreshold) b |= btn(kBtnStickLLeft);
    if (l.x > kStickButtonThreshold) b |= btn(kBtnStickLRight);
    if (l.y < -kStickButtonThreshold) b |= btn(kBtnStickLDown);
    if (l.y > kStickButtonThreshold) b |= btn(kBtnStickLUp);
    if (r.x < -kStickButtonThreshold) b |= btn(kBtnStickRLeft);
    if (r.x > kStickButtonThreshold) b |= btn(kBtnStickRRight);
    if (r.y < -kStickButtonThreshold) b |= btn(kBtnStickRDown);
    if (r.y > kStickButtonThreshold) b |= btn(kBtnStickRUp);
    return b;
}

// Ryujinx NpadDevices.SetupNpad: what a pad of this style looks like the moment it appears (or leaves). tag 0 clears the pad.
void apply_style(uint8_t* e, uint32_t tag, const NpadInput& in) {
    w32(e, kStyleSet, tag);
    if (tag == 0) {
        w32(e, kDeviceType, 0);
        w64(e, kSystemProperties, 0);
        return;
    }
    w32(e, kJoyAssignmentMode, (tag == kTagJoyLeft || tag == kTagJoyRight) ? 1 : 0);   // Single for the half pads, else Dual
    w32(e, kFullKeyColor + 0, 0);
    w32(e, kFullKeyColor + 4, in.color_body);
    w32(e, kFullKeyColor + 8, in.color_buttons);
    w32(e, kJoyColor + 0, 0);
    w32(e, kJoyColor + 4, in.color_body);
    w32(e, kJoyColor + 8, in.color_buttons);
    w32(e, kJoyColor + 12, in.color_body);
    w32(e, kJoyColor + 16, in.color_buttons);
    // NpadSystemProperties: powered Joy-Con bits 3..5 always; ABXY/Plus/Minus for the full pads, SL/SR and one of Plus/Minus for halves.
    uint64_t props = (1ull << 3) | (1ull << 4) | (1ull << 5);
    constexpr uint64_t kAbxy = 1ull << 11, kSlSr = 1ull << 12, kPlus = 1ull << 13, kMinus = 1ull << 14;
    uint32_t device = 0;
    switch (tag) {
        case kTagFullKey: props |= kAbxy | kPlus | kMinus; device = 1u << 0; break;
        case kTagHandheld: props |= kAbxy | kPlus | kMinus; device = (1u << 2) | (1u << 3); break;
        case kTagJoyDual: props |= kAbxy | kPlus | kMinus; device = (1u << 4) | (1u << 5); break;
        case kTagJoyLeft: props |= kSlSr | kMinus; device = 1u << 4; break;
        case kTagJoyRight: props |= kSlSr | kPlus; device = 1u << 5; break;
        default: break;
    }
    w32(e, kDeviceType, device);
    w64(e, kSystemProperties, props);
    w32(e, kBatteryJoyDual, 4);   // NpadBatteryLevel.Percent100
    w32(e, kBatteryJoyLeft, 4);
    w32(e, kBatteryJoyRight, 4);
}

// The stick values are the source's; the direction bits are added where the source left them clear.
void sample_npad(HidState& s, uint8_t* host, uint32_t idx, const InputSnapshot& snap, bool docked) {
    uint8_t* e = host + kNpads + idx * kNpadEntrySize;
    const NpadInput& in = snap.npad[slot_of_index(idx)];
    const uint32_t wanted = tag_of_style(in.style);
    // The handheld entry is only live in handheld mode (Ryujinx Remap); a style must be in the supported set and the id supported.
    const bool on = in.connected && wanted != 0 && (s.supported_styles & wanted) && ((s.supported_index_mask >> idx) & 1) &&
                    !(idx == 8 && docked);
    const uint32_t tag = on ? wanted : 0;
    if (tag != s.last_tag[idx]) {
        apply_style(e, tag, in);
        s.last_tag[idx] = tag;
        s.style_event[idx].signal();
    }
    s.connected[idx] = on;

    NpadCommonState real{};
    SixAxisState six{};
    if (on) {
        real.buttons = in.buttons | stick_buttons(in.left, in.right);
        real.lx = in.left.x;
        real.ly = in.left.y;
        real.rx = in.right.x;
        real.ry = in.right.y;
        real.attributes = attributes_of_tag(tag);
        six.delta_time = kSampleNs;
        six.attributes = 1;   // connected
        for (int k = 0; k < 3; ++k) {
            six.accel[k] = in.six_axis.accel[k];
            six.gyro[k] = in.six_axis.gyro[k];
            six.angle[k] = in.six_axis.angle[k];
        }
        for (int k = 0; k < 9; ++k) six.direction[k] = in.six_axis.direction[k];
    }
    // The pad's own style lifo gets the state and SystemExt mirrors it; every other common lifo gets an empty entry, so all of them keep
    // counting (Ryujinx NpadDevices.UpdateInput). A disconnected pad writes empty entries everywhere (attributes cleared).
    for (uint64_t off : kCommonLifos) {
        const bool pick = on && (off == common_lifo_of_tag(tag) || off == kSystemExtLifo);
        ring_push(ring_at<NpadCommonState>(e, off), pick ? real : NpadCommonState{});
    }
    for (uint64_t off : kSixAxisLifos) {
        const bool pick = on && six_lifo_of_tag(tag, off);
        ring_push(ring_at<SixAxisState>(e, off), pick ? six : SixAxisState{});
    }
}

void sample_touch(uint8_t* host, const nxrt::hid::TouchInput& t) {
    TouchScreenState ts{};
    const uint32_t n = std::min<uint32_t>(t.count, 16);
    ts.count = static_cast<int32_t>(n);
    for (uint32_t i = 0; i < n; ++i) {
        const nxrt::hid::TouchFinger& f = t.fingers[i];
        TouchState& d = ts.touches[i];
        d.finger = f.id;
        d.x = f.x;
        d.y = f.y;
        d.diameter_x = f.diameter_x;
        d.diameter_y = f.diameter_y;
        d.rotation_angle = static_cast<uint32_t>(f.angle);
    }
    ring_push(ring_at<TouchScreenState>(host, kTouchScreen), ts);
}

void sample_mouse(uint8_t* host, const nxrt::hid::MouseInput& m) {
    MouseState ms{};
    ms.x = m.x;
    ms.y = m.y;
    ms.dx = m.dx;
    ms.dy = m.dy;
    ms.wheel_dx = m.wheel_dx;
    ms.wheel_dy = m.wheel_dy;
    ms.buttons = m.buttons;
    ms.attributes = m.connected ? 2u : 0u;   // bit 1 connected
    ring_push(ring_at<MouseState>(host, kMouse), ms);
}

void sample_keyboard(uint8_t* host, const nxrt::hid::KeyboardInput& k) {
    KeyboardState ks{};
    ks.modifiers = k.modifiers;
    for (int i = 0; i < 4; ++i) ks.keys[i] = k.keys[i];
    ring_push(ring_at<KeyboardState>(host, kKeyboard), ks);
}

// The input for one sample: the source's, or the default pads (player 1 idle FullKey, handheld in handheld mode).
InputSnapshot read_input() {
    InputSnapshot snap;
    if (auto src = nxrt::hid::input_source()) {
        src->poll(snap);
        uint64_t b = 0;
        for (const auto& n : snap.npad)
            if (n.connected) b |= n.buttons;
        nxrt::hid::set_sampled_buttons(b);
        return snap;
    }
    nxrt::hid::set_sampled_buttons(0);
    snap.npad[0].connected = true;
    snap.npad[0].style = NpadStyle::FullKey;
    if (!profile().docked) {
        snap.npad[nxrt::hid::kNpadSlotHandheld].connected = true;
        snap.npad[nxrt::hid::kNpadSlotHandheld].style = NpadStyle::Handheld;
    }
    return snap;
}

void sampler_main(HidState* s) {
    log_set_thread_name("hid-sampler");
    while (!s->stop.load()) {
        if (s->background.load() && s->resources.load() > 0) hid_sample_once();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Caller holds st().mu.
void ensure_sampler(HidState& s) {
    if (!s.sampler.joinable()) s.sampler = std::thread(sampler_main, &s);
}

// Joy assignment of one entry, written straight into the shared memory (the sampler keeps it until the next style change).
void set_joy_assignment(HidState& s, int idx, uint32_t mode) {
    if (idx < 0) return;
    uint8_t* host = host_of(s);
    if (host) w32(host + kNpads + static_cast<uint64_t>(idx) * kNpadEntrySize, kJoyAssignmentMode, mode);
}

// Decodes a VibrationDeviceHandle / SixAxisSensorHandle: low byte device type, byte 1 NpadIdType, bit 16 position (ScarletNX hid.c,
// Ryujinx VibrationDeviceHandle).
struct DeviceHandle {
    uint32_t device_type, npad_id, position;
};
DeviceHandle decode_handle(uint32_t h) {
    return {h & 0xFF, (h >> 8) & 0xFF, (h >> 16) & 0xFF};
}

// Input slot of a handle's npad, or -1.
int slot_of_handle(uint32_t h) {
    const int idx = index_of_npad_id(decode_handle(h).npad_id);
    return idx < 0 ? -1 : static_cast<int>(slot_of_index(static_cast<uint32_t>(idx)));
}

// Forwards a stored value to the vibration sink: the handle's npad slot and its position (0 left or the only device, 1 right). Handles
// whose npad is invalid or whose position is past 1 are not forwarded. Called without st().mu held.
void forward_vibration(uint32_t h, const VibValue& v) {
    const int slot = slot_of_handle(h);
    const uint32_t hand = decode_handle(h).position;
    if (slot < 0 || hand > 1) return;
    nxrt::hid::VibrationValue out;
    out.amp_low = v.amp_low;
    out.freq_low = v.freq_low;
    out.amp_high = v.amp_high;
    out.freq_high = v.freq_high;
    vibration_dispatch(static_cast<uint32_t>(slot), static_cast<int>(hand), out);
}

// ---- commands ---------------------------------------------------------------------------------------------------------------------------
// A command that is accepted and does nothing (Switchbrew: stubbed, or nothing to do here). Writes `out_bytes` zero bytes as its reply.
void ignore_cmd(ipc::Service& svc, uint32_t id, const char* name, size_t out_bytes = 0) {
    svc.add_cmd(id, name, [name, out_bytes](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        NXLOG_D("hid", "%s: accepted and ignored", name);
        rsp.out.resize(rsp.out.size() + out_bytes, 0);
        return 0;
    });
}

// A command whose handle output is 0 on hardware (Switchbrew: "[10.0.0+] Stubbed, just returns 0").
void zero_handle_cmd(ipc::Service& svc, uint32_t id, const char* name) {
    svc.add_cmd(id, name, [name](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        NXLOG_D("hid", "%s: stubbed, returns handle 0", name);
        rsp.out_copy.push_back(0);
        return 0;
    });
}

// Gauge (include/nxrt/gauge.h): IAppletResource objects alive (the sampler runs while any is).
GaugeCounter g_hid_resources("hid.applet_resources");

// IAppletResource (nn::hid::IAppletResource): one per CreateAppletResource. While one is alive the sampler runs.
class AppletResource : public ipc::Service {
public:
    AppletResource() : ipc::Service("nn::hid::IAppletResource") {
        st().resources++;
        g_hid_resources.add(1);
        add_cmd(0, "GetSharedMemoryHandle", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            HidState& s = st();
            std::lock_guard<std::mutex> lk(s.mu);
            if (!shm_ready(s)) return kernel::kOutOfMemory;
            const uint32_t h = s.shm->new_handle();
            if (!h) return kResultNoHandle;
            rsp.out_copy.push_back(h);
            return 0;
        });
    }
    ~AppletResource() override {
        st().resources--;
        g_hid_resources.sub(1);
    }
};

std::shared_ptr<ipc::Service> make_vibration_list() {
    return make_iface("nn::hid::IActiveVibrationDeviceList", [](ipc::Service& svc) {
        ignore_cmd(svc, 0, "ActivateVibrationDevice");   // the devices are activated and never vibrate (ScarletNX hid.swipc)
    });
}

void setup_hid(ipc::Service& svc) {
    // ---- resource and the device activations
    svc.add_cmd(0, "CreateAppletResource", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        {
            std::lock_guard<std::mutex> lk(st().mu);
            ensure_sampler(st());
        }
        rsp.push_object(std::make_shared<AppletResource>());
        NXLOG_I("hid", "CreateAppletResource");
        return 0;
    });
    svc.add_cmd(1, "ActivateDebugPad", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().debug_pad_active = true;
        return 0;
    });
    svc.add_cmd(11, "ActivateTouchScreen", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().touch_active = true;
        return 0;
    });
    svc.add_cmd(21, "ActivateMouse", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().mouse_active = true;
        return 0;
    });
    svc.add_cmd(31, "ActivateKeyboard", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().keyboard_active = true;
        return 0;
    });
    ignore_cmd(svc, 32, "SendKeyboardLockKeyEvent");
    zero_handle_cmd(svc, 40, "AcquireXpadIdEventHandle");
    ignore_cmd(svc, 51, "ActivateXpad");
    ignore_cmd(svc, 55, "GetXpadIds", 8);
    ignore_cmd(svc, 56, "ActivateJoyXpad");
    zero_handle_cmd(svc, 58, "GetJoyXpadLifoHandle");
    ignore_cmd(svc, 59, "GetJoyXpadIds", 8);

    // ---- six-axis sensors (the samples are in the six-axis lifos; start and stop change nothing, as in Ryujinx)
    ignore_cmd(svc, 60, "ActivateSixAxisSensor");
    ignore_cmd(svc, 61, "DeactivateSixAxisSensor");
    zero_handle_cmd(svc, 62, "GetSixAxisSensorLifoHandle");
    ignore_cmd(svc, 63, "ActivateJoySixAxisSensor");
    ignore_cmd(svc, 64, "DeactivateJoySixAxisSensor");
    zero_handle_cmd(svc, 65, "GetJoySixAxisSensorLifoHandle");
    ignore_cmd(svc, 66, "StartSixAxisSensor");
    ignore_cmd(svc, 67, "StopSixAxisSensor");
    svc.add_cmd(68, "IsSixAxisSensorFusionEnabled", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<uint8_t>(st().sixaxis_fusion ? 1 : 0);
        return 0;
    });
    svc.add_cmd(69, "EnableSixAxisSensorFusion", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().sixaxis_fusion = req.arg<uint8_t>(0) != 0;
        return 0;
    });
    ignore_cmd(svc, 70, "SetSixAxisSensorFusionParameters");
    ignore_cmd(svc, 71, "GetSixAxisSensorFusionParameters", 8);
    ignore_cmd(svc, 72, "ResetSixAxisSensorFusionParameters");
    ignore_cmd(svc, 73, "SetAccelerometerParameters");
    ignore_cmd(svc, 74, "GetAccelerometerParameters", 8);
    ignore_cmd(svc, 75, "ResetAccelerometerParameters");
    ignore_cmd(svc, 76, "SetAccelerometerPlayMode");
    ignore_cmd(svc, 77, "GetAccelerometerPlayMode", 4);
    ignore_cmd(svc, 78, "ResetAccelerometerPlayMode");
    ignore_cmd(svc, 79, "SetGyroscopeZeroDriftMode");
    ignore_cmd(svc, 80, "GetGyroscopeZeroDriftMode", 4);
    ignore_cmd(svc, 81, "ResetGyroscopeZeroDriftMode");
    svc.add_cmd(82, "IsSixAxisSensorAtRest", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        // The handle names the pad; the answer is that pad's at_rest in the current input (true when no such pad).
        bool at_rest = true;
        const int slot = slot_of_handle(req.arg<uint32_t>(0));
        if (slot >= 0) {
            InputSnapshot snap = read_input();
            at_rest = snap.npad[slot].six_axis.at_rest;
        }
        rsp.push<uint8_t>(at_rest ? 1 : 0);
        return 0;
    });
    ignore_cmd(svc, 83, "IsFirmwareUpdateAvailableForSixAxisSensor", 1);
    ignore_cmd(svc, 91, "ActivateGesture");

    // ---- npads
    svc.add_cmd(100, "SetSupportedNpadStyleSet", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().supported_styles = req.arg<uint32_t>(0);
        return 0;
    });
    svc.add_cmd(101, "GetSupportedNpadStyleSet", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<uint32_t>(st().supported_styles);
        return 0;
    });
    svc.add_cmd(102, "SetSupportedNpadIdType", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const ipc::Buffer b = req.in_buf(0);
        const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(b.size / 4), 10);
        std::vector<uint32_t> ids(n);
        if (n) buf_read(b, ids.data(), n * 4ull);
        uint32_t mask = 0;
        for (uint32_t id : ids) {
            const int idx = index_of_npad_id(id);
            if (idx >= 0) mask |= 1u << idx;
        }
        std::lock_guard<std::mutex> lk(st().mu);
        st().supported_index_mask = mask;
        return 0;
    });
    svc.add_cmd(103, "ActivateNpad", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().npad_active = true;
        return 0;
    });
    svc.add_cmd(104, "DeactivateNpad", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().npad_active = false;
        return 0;
    });
    svc.add_cmd(106, "AcquireNpadStyleSetUpdateEventHandle", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const int idx = index_of_npad_id(req.arg<uint32_t>(0));
        if (idx < 0) return kResultInvalidNpadId;
        std::lock_guard<std::mutex> lk(st().mu);
        const uint32_t h = st().style_event[idx].new_handle();
        if (!h) return kResultNoHandle;
        st().style_event[idx].signal();   // games expect it signalled after this call (Ryujinx)
        rsp.out_copy.push_back(h);
        return 0;
    });
    ignore_cmd(svc, 107, "DisconnectNpad");
    svc.add_cmd(108, "GetPlayerLedPattern", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        // Player LED patterns as Ryujinx's table (players 1..8); handheld and other have none.
        static constexpr uint64_t kLed[8] = {0b0001, 0b0011, 0b0111, 0b1111, 0b1001, 0b0101, 0b1101, 0b0110};
        const uint32_t id = req.arg<uint32_t>(0);
        if (id >= 8 && id != kNpadIdHandheld && id != kNpadIdOther) return kResultInvalidNpadId;
        rsp.push<uint64_t>(id < 8 ? kLed[id] : 0);
        return 0;
    });
    svc.add_cmd(109, "ActivateNpadWithRevision", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().npad_active = true;
        return 0;
    });
    svc.add_cmd(120, "SetNpadJoyHoldType", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().joy_hold = req.arg<uint64_t>(8);
        return 0;
    });
    svc.add_cmd(121, "GetNpadJoyHoldType", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<uint64_t>(st().joy_hold);
        return 0;
    });
    svc.add_cmd(122, "SetNpadJoyAssignmentModeSingleByDefault", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        set_joy_assignment(st(), index_of_npad_id(req.arg<uint32_t>(0)), 1);
        return 0;
    });
    svc.add_cmd(123, "SetNpadJoyAssignmentModeSingle", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        set_joy_assignment(st(), index_of_npad_id(req.arg<uint32_t>(0)), 1);
        return 0;
    });
    svc.add_cmd(124, "SetNpadJoyAssignmentModeDual", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        set_joy_assignment(st(), index_of_npad_id(req.arg<uint32_t>(0)), 0);
        return 0;
    });
    ignore_cmd(svc, 125, "MergeSingleJoyAsDualJoy");
    ignore_cmd(svc, 126, "StartLrAssignmentMode");
    ignore_cmd(svc, 127, "StopLrAssignmentMode");
    svc.add_cmd(128, "SetNpadHandheldActivationMode", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().handheld_activation = req.arg<int64_t>(8);
        return 0;
    });
    svc.add_cmd(129, "GetNpadHandheldActivationMode", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<int64_t>(st().handheld_activation);
        return 0;
    });
    ignore_cmd(svc, 130, "SwapNpadAssignment");
    svc.add_cmd(131, "IsUnintendedHomeButtonInputProtectionEnabled", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<uint8_t>(st().unintended_home_protection ? 1 : 0);
        return 0;
    });
    svc.add_cmd(132, "EnableUnintendedHomeButtonInputProtection", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().unintended_home_protection = req.arg<uint8_t>(0) != 0;
        return 0;
    });
    svc.add_cmd(133, "SetNpadJoyAssignmentModeSingleWithDestination", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        // Output: bool "is reassigned" and the new id. Nothing is ever merged here, so it is always false and 0.
        std::lock_guard<std::mutex> lk(st().mu);
        set_joy_assignment(st(), index_of_npad_id(req.arg<uint32_t>(0)), 1);
        rsp.push<uint8_t>(0);
        rsp.push<uint8_t>(0);
        rsp.push<uint8_t>(0);
        rsp.push<uint8_t>(0);
        rsp.push<uint32_t>(0);
        return 0;
    });
    ignore_cmd(svc, 134, "SetNpadAnalogStickUseCenterClamp");

    // ---- vibration
    svc.add_cmd(200, "GetVibrationDeviceInfo", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const DeviceHandle d = decode_handle(req.arg<uint32_t>(0));
        if (index_of_npad_id(d.npad_id) < 0) return kResultInvalidNpadId;
        if (d.position > 1) return kResultInvalidDeviceIndex;
        // Ryujinx: device types 3..7 and 32, 33 (NpadStyleIndex) are linear resonant actuators, 8 is a GC ERM; the position of an LRA is
        // 0 = left, 1 = right. VibrationDeviceType: None 0, LinearResonantActuator 1, GcErm 2. VibrationDevicePosition: None 0, Left 1, Right 2.
        const bool lra = d.device_type == 3 || d.device_type == 4 || d.device_type == 5 || d.device_type == 6 || d.device_type == 7 ||
                         d.device_type == 32 || d.device_type == 33;
        const uint32_t type = lra ? 1 : (d.device_type == 8 ? 2 : 0);
        const uint32_t pos = (lra && d.position <= 1) ? d.position + 1 : 0;
        rsp.push<uint32_t>(type);
        rsp.push<uint32_t>(pos);
        return 0;
    });
    svc.add_cmd(201, "SendVibrationValue", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        // Handle (u32), then the VibrationValue (4 floats) right after it. The aruid that follows is not needed here.
        const uint32_t h = req.arg<uint32_t>(0);
        VibValue v;
        v.amp_low = req.arg<float>(4);
        v.freq_low = req.arg<float>(8);
        v.amp_high = req.arg<float>(12);
        v.freq_high = req.arg<float>(16);
        {
            std::lock_guard<std::mutex> lk(st().mu);
            st().vibration[h] = v;
        }
        forward_vibration(h, v);
        return 0;
    });
    svc.add_cmd(202, "GetActualVibrationValue", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const uint32_t h = req.arg<uint32_t>(0);
        std::lock_guard<std::mutex> lk(st().mu);
        auto it = st().vibration.find(h);
        const VibValue v = it == st().vibration.end() ? VibValue{} : it->second;
        rsp.push<float>(v.amp_low);
        rsp.push<float>(v.freq_low);
        rsp.push<float>(v.amp_high);
        rsp.push<float>(v.freq_high);
        return 0;
    });
    svc.add_cmd(203, "CreateActiveVibrationDeviceList", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_vibration_list());
        return 0;
    });
    svc.add_cmd(204, "PermitVibration", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().vibration_permitted = req.arg<uint8_t>(0) != 0;
        return 0;
    });
    svc.add_cmd(205, "IsVibrationPermitted", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<uint8_t>(st().vibration_permitted ? 1 : 0);
        return 0;
    });
    svc.add_cmd(206, "SendVibrationValues", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        // Two buffers: an array of handles (u32) and an array of VibrationValue (16 bytes each), same entry count.
        const ipc::Buffer hb = req.in_buf(0), vb = req.in_buf(1);
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(std::min<uint64_t>(hb.size / 4, vb.size / 16), 16));
        std::vector<uint32_t> handles(n);
        std::vector<float> vals(n * 4ull);
        if (n) {
            buf_read(hb, handles.data(), n * 4ull);
            buf_read(vb, vals.data(), n * 16ull);
        }
        std::vector<VibValue> values(n);
        for (uint32_t i = 0; i < n; ++i) {
            values[i].amp_low = vals[i * 4 + 0];
            values[i].freq_low = vals[i * 4 + 1];
            values[i].amp_high = vals[i * 4 + 2];
            values[i].freq_high = vals[i * 4 + 3];
        }
        {
            std::lock_guard<std::mutex> lk(st().mu);
            for (uint32_t i = 0; i < n; ++i) st().vibration[handles[i]] = values[i];
        }
        for (uint32_t i = 0; i < n; ++i) forward_vibration(handles[i], values[i]);
        return 0;
    });
    ignore_cmd(svc, 207, "SendVibrationGcErmCommand");
    ignore_cmd(svc, 208, "GetActualVibrationGcErmCommand", 8);
    ignore_cmd(svc, 209, "BeginPermitVibrationSession");
    ignore_cmd(svc, 210, "EndPermitVibrationSession");
    svc.add_cmd(211, "IsVibrationDeviceMounted", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        const int idx = index_of_npad_id(decode_handle(req.arg<uint32_t>(0)).npad_id);
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<uint8_t>(idx >= 0 && st().connected[idx] ? 1 : 0);
        return 0;
    });
    ignore_cmd(svc, 212, "SendVibrationValueInBool");
    svc.add_cmd(214, "SendVibrationValues2", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        // Switch 2: nn::hid::detail::SendVibrationValues (sdk 0x750064d360) sends this instead of 206 unless the Switch 1 compatible
        // mode is set, with the same two buffers (handles, VibrationValues) and an extra u16 (2). Forwarded like 206.
        const ipc::Buffer hb = req.in_buf(0), vb = req.in_buf(1);
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(std::min<uint64_t>(hb.size / 4, vb.size / 16), 16));
        std::vector<uint32_t> handles(n);
        std::vector<VibValue> values(n);
        if (n) {
            buf_read(hb, handles.data(), n * 4ull);
            buf_read(vb, values.data(), n * 16ull);
        }
        {
            std::lock_guard<std::mutex> lk(st().mu);
            for (uint32_t i = 0; i < n; ++i) st().vibration[handles[i]] = values[i];
            if (!st().vib214_logged && n) {
                st().vib214_logged = true;
                NXLOG_I("hid", "SendVibrationValues2 (214): %u values, first handle %x, routed to the pad rumble", n, handles[0]);
            }
        }
        for (uint32_t i = 0; i < n; ++i) forward_vibration(handles[i], values[i]);
        return 0;
    });

    // ---- console and seven-axis sensors (not used by the title; accepted)
    ignore_cmd(svc, 300, "ActivateConsoleSixAxisSensor");
    ignore_cmd(svc, 301, "StartConsoleSixAxisSensor");
    ignore_cmd(svc, 302, "StopConsoleSixAxisSensor");
    ignore_cmd(svc, 303, "ActivateSevenSixAxisSensor");
    ignore_cmd(svc, 304, "StartSevenSixAxisSensor");
    ignore_cmd(svc, 305, "StopSevenSixAxisSensor");
    ignore_cmd(svc, 306, "InitializeSevenSixAxisSensor");
    ignore_cmd(svc, 307, "FinalizeSevenSixAxisSensor");
    ignore_cmd(svc, 308, "SetSevenSixAxisSensorFusionStrength");
    ignore_cmd(svc, 309, "GetSevenSixAxisSensorFusionStrength", 4);
    ignore_cmd(svc, 310, "ResetSevenSixAxisSensorTimestamp");

    // ---- system and mode
    svc.add_cmd(400, "IsUsbFullKeyControllerEnabled", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(0);
        return 0;
    });
    svc.add_cmd(1000, "SetNpadCommunicationMode", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        // The mode comes first, then the aruid (Ryujinx). ScarletNX's table lists the aruid first; see the report.
        std::lock_guard<std::mutex> lk(st().mu);
        st().npad_communication = req.arg<int64_t>(0);
        return 0;
    });
    svc.add_cmd(1001, "GetNpadCommunicationMode", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<int64_t>(st().npad_communication);
        return 0;
    });
    ignore_cmd(svc, 1002, "SetTouchScreenConfiguration");
    ignore_cmd(svc, 1003, "IsFirmwareUpdateNeededForNotification", 1);
    ignore_cmd(svc, 1004, "SetTouchScreenResolution");
    // ---- Switch 2 Joy-Con 2 mouse (Nmouse / Jcms). Ids from the sdk proxy thunks (0x75006701b4..0x75006702ec, IHidServer vtable
    // slots 0x538..0x5B8) matched to the nn::hid::detail callers; raw-data layouts from the thunks' CMIF writers (the local Joy-Con 2 mouse notes).
    // Pokopia imports only the wrappers behind 3110, 3111, 3112, 3131 and 3136-3139; the others get the answer a server with the two
    // listed devices gives. There is no 3114-3129 or 3134 in the SDK's table.
    svc.add_cmd(3110, "InitializeJcms", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const uint64_t size = req.arg<uint64_t>(8);   // aruid at 0
        std::lock_guard<std::mutex> lk(st().mu);
        HidState& s = st();
        s.jcms_addr = tm_addr(req, size, kJcmsMinSize, "InitializeJcms");
        s.jcms_size = s.jcms_addr ? size : 0;
        if (s.jcms_addr) jcms_setup(s);
        NXLOG_I("hid", "InitializeJcms(size %llx) -> block %llx", static_cast<unsigned long long>(size), static_cast<unsigned long long>(s.jcms_addr));
        return 0;
    });
    svc.add_cmd(3111, "FinalizeJcms", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().jcms_addr = st().jcms_size = 0;
        NXLOG_I("hid", "FinalizeJcms");
        return 0;
    });
    svc.add_cmd(3112, "StartJcms", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        // u32 JcmsHandle @0, aruid @8. The game calls it every frame for each Joy-Con 2 mouse it found, so a call proves it read the
        // Nmouse handle table. The Jcms rings are sampled whether started or not.
        std::lock_guard<std::mutex> lk(st().mu);
        if (st().jcms_starts < 3) NXLOG_I("hid", "StartJcms(handle %x): the game found the Joy-Con 2 mouse", req.arg<uint32_t>(0));
        st().jcms_starts++;
        return 0;
    });
    // u32 JcmsHandle @0, aruid @8. Not imported by Pokopia; the rings keep sampling (nothing to stop).
    ignore_cmd(svc, 3113, "StopJcms");
    // InitializeNmouse with an NmouseStateRange (sdk 0x75006576c0): the width and height come from the SDK's range table (720p, 1080p,
    // 1440p, 4K), the pointer type is 0. The SDK sends SetMergedNmouseSourceTypes(6) right after it.
    svc.add_cmd(3130, "InitializeNmouseWithStateRange", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        return nm_initialize(req, "InitializeNmouseWithStateRange");
    });
    svc.add_cmd(3131, "FinalizeNmouse", [](ipc::Request&, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().nm_addr = st().nm_size = 0;
        g_jc2_listed.store(false);
        g_jc2_active.store(false);
        NXLOG_I("hid", "FinalizeNmouse");
        return 0;
    });
    // SetNmousePointerRanges / SetNmouseStateRanges (sdk 0x7500656cc0, 0x75006578c0): s32 width @0, s32 height @4, aruid @8. The new
    // range applies from the next sample (pointer position and the state's range fields).
    svc.add_cmd(3132, "SetNmousePointerRanges", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const int32_t w = req.arg<int32_t>(0), h = req.arg<int32_t>(4);
        std::lock_guard<std::mutex> lk(st().mu);
        if (w > 0) st().nm_w = w;
        if (h > 0) st().nm_h = h;
        NXLOG_I("hid", "SetNmousePointerRanges(%d, %d)", w, h);
        return 0;
    });
    // GetNmousePointerRanges / GetNmouseStateRanges (sdk 0x7500656d40, 0x7500657960): aruid in; out s32 width @0, s32 height @4.
    svc.add_cmd(3133, "GetNmousePointerRanges", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        rsp.push<int32_t>(st().nm_w);
        rsp.push<int32_t>(st().nm_h);
        return 0;
    });
    // EnableShowingCursor (sdk 0x750065719c): bool @0, aruid @8. Stored where the client reads it back
    // (NmouseTransferMemoryAccessor::IsEnabledShowingCursor, block +0x8E68).
    svc.add_cmd(3135, "EnableShowingCursor", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        if (st().nm_addr) reinterpret_cast<uint8_t*>(st().nm_addr)[kNmShowCursor] = req.arg<uint8_t>(0) ? 1 : 0;
        return 0;
    });
    svc.add_cmd(3136, "SetNmouseButtonConfigJoyLeftJcms", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().nm_btn_left = req.arg<uint64_t>(8);   // aruid at 0
        return 0;
    });
    svc.add_cmd(3137, "SetNmouseButtonConfigJoyRightJcms", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().nm_btn_right = req.arg<uint64_t>(8);
        return 0;
    });
    svc.add_cmd(3138, "SetMergedNmouseSourceTypes", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> lk(st().mu);
        st().nm_merged_types = req.arg<uint32_t>(0);   // aruid at 8
        return 0;
    });
    // InitializeNmouse (sdk 0x75006569e0, the form Pokopia uses: 0xC000 bytes, 1920x1080).
    svc.add_cmd(3139, "InitializeNmouse", [](ipc::Request& req, ipc::Response&) -> uint32_t { return nm_initialize(req, "InitializeNmouse"); });
    // SetNmousePointerPosition (sdk 0x7500657410): s32 x @0, s32 y @4, aruid @8, NmouseHandle @0x10. The pointer is the PC mouse, which
    // the game cannot move: accepted and logged.
    svc.add_cmd(3140, "SetNmousePointerPosition", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        NXLOG_I("hid", "SetNmousePointerPosition(%d, %d, handle %llx): the pointer is the PC mouse, not moved", req.arg<int32_t>(0),
                req.arg<int32_t>(4), static_cast<unsigned long long>(req.arg<uint64_t>(0x10)));
        return 0;
    });
    // EnableNmousePointer (sdk 0x75006572b0): bool @0, aruid @8, NmouseHandle @0x10. Stored in that handle's info byte, which the client
    // reads back (IsEnableNmousePointer, info +0x10).
    svc.add_cmd(3141, "EnableNmousePointer", [](ipc::Request& req, ipc::Response&) -> uint32_t {
        const bool on = req.arg<uint8_t>(0) != 0;
        const uint64_t handle = req.arg<uint64_t>(0x10);
        std::lock_guard<std::mutex> lk(st().mu);
        HidState& s = st();
        if (!s.nm_addr) return 0;
        uint8_t* b = reinterpret_cast<uint8_t*>(s.nm_addr);
        for (uint32_t i = 0; i < kNmSlots; ++i)
            if (b[kNmHandles + i * 16 + 8] && std::memcmp(b + kNmHandles + i * 16, &handle, 8) == 0) b[kNmInfo + i * kNmInfoSize + 0x10] = on ? 1 : 0;
        return 0;
    });
    // RestrictNmousePointer (sdk 0x7500657630): four floats @0..0xC, aruid @0x10. The PC pointer is not restricted: accepted.
    ignore_cmd(svc, 3142, "RestrictNmousePointer");
    // 3143 (vtable slot 0x5B8): aruid @0, u64 @8; no nn::hid::detail caller in the SDK, name unknown. Accepted.
    ignore_cmd(svc, 3143, "Cmd3143");
}

void hid_stop_sampler(HidState& s) {
    s.stop.store(true);
    if (s.sampler.joinable()) s.sampler.join();
    s.stop.store(false);
}

}  // namespace


void register_hid() {
    add("hid", [] { return make_iface("nn::hid::IHidServer", setup_hid); });
}

void hid_reset() {
    HidState& s = st();
    hid_stop_sampler(s);
    std::lock_guard<std::mutex> lk(s.mu);
    s.supported_styles = kDefaultStyles;
    s.supported_index_mask = kDefaultIdMask;
    s.npad_active = s.debug_pad_active = s.touch_active = s.mouse_active = s.keyboard_active = false;
    s.sixaxis_fusion = false;
    s.vibration_permitted = true;
    s.unintended_home_protection = false;
    s.joy_hold = 0;
    s.handheld_activation = 0;
    s.npad_communication = 0;
    std::memset(s.last_tag, 0, sizeof s.last_tag);
    std::memset(s.connected, 0, sizeof s.connected);
    s.vibration.clear();
    s.nm_addr = s.nm_size = s.jcms_addr = s.jcms_size = 0;
    s.nm_w = 1920;
    s.nm_h = 1080;
    s.nm_merged_types = 0;
    s.nm_btn_left = s.nm_btn_right = 0;
    s.nm_x = s.nm_y = -1;
    s.nm_logged = s.jcms_starts = s.active_logs = 0;
    s.vib214_logged = false;
    g_jc2_listed.store(false);
    g_jc2_active.store(false);
    s.background.store(true);
    for (auto& e : s.style_event) e = ServiceEvent();
    if (s.shm && s.shm->ok() && mem::is_mapped(s.shm->host(), kSize)) {
        init_shm(reinterpret_cast<uint8_t*>(s.shm->host()));
    } else {
        s.shm.reset();   // created again on first use
    }
}

void hid_sample_once() {
    HidState& s = st();
    InputSnapshot snap = read_input();
    snap.npad[0].buttons |= jc2_pad_buttons();
    const bool docked = profile().docked;
    std::lock_guard<std::mutex> lk(s.mu);
    uint8_t* host = host_of(s);
    if (!host) return;
    if (s.npad_active) {
        for (uint32_t i = 0; i < kNpadCount; ++i) sample_npad(s, host, i, snap, docked);
    }
    if (s.debug_pad_active) ring_push(ring_at<DebugPadState>(host, kDebugPad), DebugPadState{});
    if (s.touch_active) sample_touch(host, snap.touch);
    if (s.mouse_active) sample_mouse(host, snap.mouse);
    if (s.keyboard_active) sample_keyboard(host, snap.keyboard);
    sample_nmouse(s);
}

void hid_set_background_sampling(bool enabled) {
    st().background.store(enabled);
}

}  // namespace nxrt::services
