// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to the HID service (hid.cpp, hid_jc2_mouse.cpp): the server state, the ring lifo helpers and the Joy-Con 2 mouse (Nmouse / Jcms)
// layouts and entry points.
#pragma once
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <map>

#include "hid_shm.h"
#include "nxrt/input_source.h"
#include "nxrt/jc2_mouse.h"
#include "services_internal.h"

namespace nxrt::services::hidsvc {

using namespace hidshm;
using nxrt::hid::InputSnapshot;
using nxrt::hid::NpadInput;
using nxrt::hid::NpadStyle;
using nxrt::hid::StickValue;

constexpr uint32_t kNpadIdOther = 0x10;       // NpadIdType (Switchbrew): players 0..7, other 0x10, handheld 0x20
constexpr uint32_t kNpadIdHandheld = 0x20;
constexpr uint32_t kDefaultStyles = kTagFullKey | kTagHandheld | kTagJoyDual | kTagJoyLeft | kTagJoyRight;
constexpr uint32_t kDefaultIdMask = 0x1FF;    // shared-memory entries 0..8: players 1..8 and handheld (Ryujinx MaxControllers 9)
constexpr int32_t kStickButtonThreshold = 32767 / 2;    // Ryujinx Hid.cs: short.MaxValue / 2
constexpr uint64_t kSampleNs = 5000000;       // 5 ms between samples (200 Hz), in ns (ScarletNX hid_motion.h dt_ns)
constexpr uint32_t kResultInvalidNpadId = res(202, 123);       // Ryujinx ResultCode.InvalidNpadIdType
constexpr uint32_t kResultInvalidDeviceIndex = res(202, 124);  // Ryujinx ResultCode.InvalidDeviceIndex
constexpr uint32_t kResultNoHandle = kernel::kOutOfHandles;    // the handle table is full

struct VibValue {
    float amp_low = 0, freq_low = 0, amp_high = 0, freq_high = 0;   // Switchbrew VibrationValue field order
};

// ---- Joy-Con 2 mouse (Nmouse / Jcms) ------------------------------------------------------------------------------------------------
// Layout from the SDK 20.5 client (sdk nn::hid::detail::NmouseTransferMemoryAccessor, GetJcmsStates); the local Joy-Con 2 mouse notes have the
// addresses. Both blocks are the game's own buffers, lent as transfer memory (InitializeNmouse 3139, InitializeJcms 3110); the server
// owns their contents and the game reads them directly.
constexpr uint32_t kNmSlots = 17;              // handle table entries, info entries and rings
constexpr uint64_t kNmHandles = 0x0;           // 17 x {u64 handle, u8 valid, pad} (16 bytes)
constexpr uint64_t kNmRings = 0x220;           // 17 rings of 0x818 (0x20 header + 17 x (u64 seq, 0x70 state))
constexpr uint64_t kNmRingSize = 0x818;
constexpr uint64_t kNmInfo = 0x8C40;           // 17 x 0x1C: u32 NmouseType set, u8 valid, u32 NpadIdType, u8 valid, u8 pointer enabled
constexpr uint64_t kNmInfoSize = 0x1C;
constexpr uint64_t kNmShowCursor = 0x8E68;     // u8 IsEnabledShowingCursor
constexpr uint64_t kNmMinSize = 0x8E70;
constexpr uint64_t kJcmsRingSize = 0x2C8;      // per JcmsHandle index (npad * 2 + type): 0x20 header + 17 x (u64 seq, 0x20 state)
constexpr uint64_t kJcmsMinSize = 16 * kJcmsRingSize;
// NmouseType bits (from the client: GetNmouseHandles asks for exactly 2 then exactly 4, the game asks 4 for its "right" side and 2 for
// "left", GetMergedNmouseHandle takes the first entry with bit 0).
constexpr uint32_t kNmTypeMerged = 1u << 0, kNmTypeJoyLeftJcms = 1u << 1, kNmTypeJoyRightJcms = 1u << 2;
constexpr uint64_t kNmHandleMerged = 0x4E4D0001, kNmHandleRight = 0x4E4D0002;   // opaque, only compared
constexpr uint32_t kNmAttrConnected = 1u << 1;  // the game counts frames with this bit to switch to mouse controls

// The server-side Nmouse state (0x70). The public NmouseState (0x30) is bytes 0..0x24, then 0x30 (at 0x24), 0x28, 0x2C
// (sdk NmouseTransferMemoryAccessor::GetNmouseStates 0x7500658980). The SDK's DevNmouseState conversion (sdk 0x7500657dc0) reads
// public +0x18/+0x1C as the pointer range and maps it to 720p/1080p/1440p/4K, so there is no movement delta in this state.
struct NmouseInternal {
    uint64_t unknown0;      // copied out, never read by the game or the SDK
    uint64_t sampling;      // +0x08
    int32_t x, y;           // +0x10 pointer in the pointer range (1920x1080 for this game); the game's cursor reads these
    int32_t range_w, range_h;   // +0x18 the pointer range (InitializeNmouse / SetNmousePointerRanges)
    int32_t wheel_x;        // +0x20 public +0x20; the game sums public +0x20 and +0x24 per sample (clamped to +-600)
    int32_t dev24;          // +0x24 not in the public state
    uint32_t buttons;       // +0x28 MouseButton bits (inferred: no reader in the game or the SDK interprets them)
    uint32_t attributes;    // +0x2C bit 1: the mouse is on its surface (the SDK's dev state turns it into a bool)
    int32_t wheel_y;        // +0x30 public +0x24 (GetWheelDeltaDev returns +0x20 and +0x30)
    uint8_t dev[0x3C];
};
static_assert(sizeof(NmouseInternal) == 0x70, "NmouseInternal");
static_assert(sizeof(Ring<NmouseInternal>) == kNmRingSize, "nmouse lifo");
struct JcmsState {
    uint64_t unknown0;
    uint64_t sampling;      // +0x08
    uint64_t unknown10;     // read by the placement code (non-zero turns the player toward the cursor); left 0
    uint64_t unknown18;
};
static_assert(sizeof(Ring<JcmsState>) == kJcmsRingSize, "jcms lifo");

struct Jc2Feed {
    std::mutex mu;
    nxrt::hid::HostMouse m;
    uint64_t last_ns = 0;
    float wheel_x = 0, wheel_y = 0;     // notches not yet sampled
};
Jc2Feed& feed();
extern std::atomic<int> g_jc2_mode;          // -1: not read from the config yet
extern std::atomic<bool> g_jc2_request;
extern std::atomic<bool> g_jc2_listed;    // the game set up Nmouse and the device is in its handle table
extern std::atomic<bool> g_jc2_active;    // last sampled "on its surface"
extern std::atomic<uint64_t> g_jc2_space; // jc2_mouse_set_pointer_space: w << 32 | h, 0 = the pointer range
uint64_t now_ns();
bool jc2_feed_live();

struct HidState {
    std::mutex mu;
    std::optional<ServiceSharedMemory> shm;
    ServiceEvent style_event[kNpadCount];
    uint32_t supported_styles = kDefaultStyles;
    uint32_t supported_index_mask = kDefaultIdMask;
    bool npad_active = false;
    bool debug_pad_active = false, touch_active = false, mouse_active = false, keyboard_active = false;
    bool sixaxis_fusion = false;
    bool vibration_permitted = true;
    bool unintended_home_protection = false;
    uint64_t joy_hold = 0;                  // NpadJoyHoldType: 0 Vertical, 1 Horizontal
    int64_t handheld_activation = 0;
    int64_t npad_communication = 0;
    uint32_t last_tag[kNpadCount] = {};     // style tag each entry had at the last sample
    bool connected[kNpadCount] = {};
    std::map<uint32_t, VibValue> vibration; // last value sent per VibrationDeviceHandle
    // Joy-Con 2 mouse
    uint64_t nm_addr = 0, nm_size = 0;      // Nmouse transfer memory (guest address = host pointer), 0 when not set up
    int32_t nm_w = 1920, nm_h = 1080;       // pointer range from InitializeNmouse
    uint64_t jcms_addr = 0, jcms_size = 0;  // Jcms transfer memory
    uint32_t nm_merged_types = 0;
    uint64_t nm_btn_left = 0, nm_btn_right = 0;   // NmouseButtonConfigJoy{Left,Right}Jcms (8 bytes each), stored only
    int32_t nm_x = -1, nm_y = -1;           // last sampled pointer (-1: none yet)
    uint32_t nm_logged = 0, jcms_starts = 0, active_logs = 0;
    bool vib214_logged = false;
    std::atomic<int> resources{0};          // live IAppletResource objects
    std::atomic<bool> background{true};     // the thread samples only when this is set
    std::atomic<bool> stop{false};
    std::thread sampler;
};

// Never destroyed: the sampler thread is joined by hid_reset(), so no static destructor has to deal with it.
HidState& st();

// ---- ring lifos -----------------------------------------------------------------------------------------------------------------------
template <class T>
Ring<T>* ring_at(uint8_t* base, uint64_t off) {
    return reinterpret_cast<Ring<T>*>(base + off);
}

inline void set_sampling(NpadCommonState& v, uint64_t n) { v.sampling = n; }
inline void set_sampling(SixAxisState& v, uint64_t n) { v.sampling = n; }
inline void set_sampling(TouchScreenState& v, uint64_t n) { v.sampling = n; }
inline void set_sampling(MouseState& v, uint64_t n) { v.sampling = n; }
inline void set_sampling(KeyboardState& v, uint64_t n) { v.sampling = n; }
inline void set_sampling(DebugPadState& v, uint64_t n) { v.sampling = n; }
inline void set_sampling(NmouseInternal& v, uint64_t n) { v.sampling = n; }
inline void set_sampling(JcmsState& v, uint64_t n) { v.sampling = n; }

// Appends one entry: the state's sampling number = the previous one + 1 (the first entry is 1), the header index moves to it, and
// count grows up to kRingMaxCount (Ryujinx RingLifo.Write). The entry's own number (AtomicStorage) is a sequence lock: odd while the
// state is being written, then twice the state's number. The SDK 20 reader (nn::hid::detail::GetNpadStates, sdk+0x5087a0) retries
// while it is odd and while it changed across the copy, and checks that neighbouring states' numbers differ by 1; older readers only
// compare it before and after the copy (libnx _hidGetStates), so the doubled value suits both.
template <class T>
void ring_push(Ring<T>* r, T value) {
    const uint64_t n = r->entries[r->index].sampling / 2 + 1;
    set_sampling(value, n);
    const uint64_t next = (r->index + 1) % kRingEntries;
    std::atomic_ref<uint64_t> seq(r->entries[next].sampling);
    seq.store(2 * n - 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    r->entries[next].state = value;
    seq.store(2 * n, std::memory_order_release);
    std::atomic_ref<uint64_t>(r->index).store(next, std::memory_order_release);
    if (r->count < kRingMaxCount) r->count++;
}

template <class T>
void ring_init(uint8_t* base, uint64_t off) {
    Ring<T>* r = ring_at<T>(base, off);
    r->buffer_count = kRingEntries;   // RingLifo.Create(): the rest is zero
}

inline void w32(uint8_t* p, uint64_t off, uint32_t v) { std::memcpy(p + off, &v, 4); }
inline void w64(uint8_t* p, uint64_t off, uint64_t v) { std::memcpy(p + off, &v, 8); }

// ---- hid_jc2_mouse.cpp
void jcms_setup(HidState& s);
void sample_nmouse(HidState& s);
uint64_t jc2_pad_buttons();    // player 1's npad buttons from the Joy-Con 2 mouse's clicks (R, ZR), 0 while it is off its surface
uint64_t tm_addr(const ipc::Request& req, uint64_t size, uint64_t need, const char* what);
uint32_t nm_initialize(ipc::Request& req, const char* what);

}  // namespace nxrt::services::hidsvc
