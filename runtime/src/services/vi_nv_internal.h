// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to the vi and nvdrv services (vi_*.cpp, vi_nv*.cpp): tags, result codes, the shared helpers, the state tables vi_nv_reset() clears
// and the root-service setups register_vi_nv() registers.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "services_internal.h"

namespace nxrt::services::vinv {

constexpr const char* kTagVi = "vi";
constexpr const char* kTagNv = "nvdrv";

// Horizon results of the vi module (114): operation failed 0x272, not found 0xE72 (ScarletNX vi.c RC(114,1), RC(114,7)).
constexpr uint32_t kViOperationFailed = 0x272;
constexpr uint32_t kViNotFound = 0xE72;

// NvError values the nvdrv replies carry (Ryujinx Nv/Types/NvResult.cs; Switchbrew NV_services uses the same numbers).
constexpr uint32_t kNvOk = 0;
constexpr uint32_t kNvNotImplemented = 1;
constexpr uint32_t kNvNotSupported = 2;
constexpr uint32_t kNvNotInitialized = 3;
constexpr uint32_t kNvInvalidParameter = 4;
constexpr uint32_t kNvInsufficientMemory = 6;
constexpr uint32_t kNvInvalidSize = 10;
constexpr uint32_t kNvInvalidValue = 11;
constexpr uint32_t kNvFileOperationFailed = 0x30003;
constexpr uint32_t kNvNotAvailableInProduction = 0x30006;
constexpr uint32_t kSyncptCount = 192;   // the Tegra X1's hardware syncpoints

// android::status_t values the buffer queue answers with (as uint32 words in the reply parcel)
constexpr uint32_t kBqOk = 0;
constexpr uint32_t kBqEBusy = static_cast<uint32_t>(-16);
constexpr uint32_t kBqNoInit = static_cast<uint32_t>(-19);
constexpr uint32_t kBqBadValue = static_cast<uint32_t>(-22);

constexpr uint32_t kLayerSlots = 8;        // a layer's binder id is its slot + 1
constexpr uint64_t kStrayLayerBase = 0x10000;
constexpr uint32_t kNativeWindowSize = 0x3c;

// A byte span the nvdrv ioctl code reads and writes (a guest buffer, or a copy of one)
struct Arg {
    uint8_t* p = nullptr;
    uint64_t n = 0;
    uint32_t u32(uint64_t off) const {
        uint32_t v = 0;
        if (p && off + 4 <= n) std::memcpy(&v, p + off, 4);
        return v;
    }
    uint64_t u64(uint64_t off) const {
        uint64_t v = 0;
        if (p && off + 8 <= n) std::memcpy(&v, p + off, 8);
        return v;
    }
    void put32(uint64_t off, uint32_t v) const {
        if (p && off + 4 <= n) std::memcpy(p + off, &v, 4);
    }
    void put64(uint64_t off, uint64_t v) const {
        if (p && off + 8 <= n) std::memcpy(p + off, &v, 8);
    }
};

Arg arg_of(const ipc::Buffer& b);
// The NUL-terminated string a guest buffer holds (at most the buffer's size).
std::string c_string(const ipc::Buffer& b);
// Logs a binder or nvdrv condition once per code (a game may call DequeueBuffer in a loop).
void warn_once(uint32_t code, const char* what);
// Registers a command that is accepted and does nothing (logged at Debug).
void accept_cmd(ipc::Service& s, uint32_t id, const char* name, const char* tag);

struct Layer {
    bool used = false;
    uint64_t layer_id = 0;
    bool connected = false;
    uint32_t buffer_count = 2;
    uint32_t width = 0, height = 0;
    ServiceEvent event;   // signalled when a buffer comes free: never, since nothing is ever queued (see DequeueBuffer)
};

struct ViState {
    std::mutex mu;
    Layer layers[kLayerSlots];
    uint64_t next_stray = kStrayLayerBase;
    ServiceEvent vsync;
    bool vsync_on = false;
    bool vsync_stop = false;
    std::condition_variable cv;
    std::thread th;
};
ViState& vi();

enum class Dev { NvMap, Ctrl, CtrlGpu, AsGpu, Gpu, Other };

struct NvMapObj {
    uint32_t size = 0, align = 0, heap = 0, flags = 0, kind = 0;
    uint64_t addr = 0;
    bool allocated = false;
    uint32_t refs = 1;
};

struct NvFile {
    Dev dev = Dev::Other;
    std::map<uint32_t, std::shared_ptr<ServiceEvent>> events;   // QueryEvent: one event per slot or id
    uint64_t user_data = 0;                                     // gpu channel: SET_USER_DATA
    uint32_t syncpt = 0;                                        // gpu channel: its syncpoint (ALLOC_GPFIFO_EX2)
    uint64_t big_page = 0x20000;                                // as-gpu: INITIALIZE_EX
    std::map<uint64_t, uint64_t> ranges;                        // as-gpu: start -> size of each reservation and mapping
};

struct NvState {
    std::mutex mu;
    bool initialized = false;
    uint32_t next_fd = 1;
    uint32_t next_handle = 1;
    uint32_t next_syncpt = 1;
    std::map<uint32_t, std::shared_ptr<NvFile>> files;
    std::map<uint32_t, NvMapObj> maps;
    std::set<uint64_t> seen;     // ioctls logged once at Info
    std::set<uint64_t> warned;   // unhandled ioctls warned once
    bool submit_logged = false;
};
NvState& nv();

// vi_display.cpp
void vsync_stop_and_join();
uint32_t vsync_new_handle();
void setup_vi_u(ipc::Service& s);
void setup_vi_s(ipc::Service& s);
void setup_vi_m(ipc::Service& s);
// vi_nvdrv.cpp
void setup_nvdrv(ipc::Service& s);

}  // namespace nxrt::services::vinv
