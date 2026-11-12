// SPDX-License-Identifier: GPL-3.0-or-later
// vi:u, vi:s, vi:m: the display services and the binder driver of a layer's buffer queue (see vi_nv.cpp for the provenance).
#include "vi_nv_internal.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/services_api.h"
#include "services_internal.h"

namespace nxrt::services::vinv {

namespace {

// ============================================================ vi: displays and layers

// nn::vi::DisplayName ids: the names Switchbrew lists, in its order ("Default" is display 0).
bool display_id_of(const char* name40, uint64_t* id) {
    static const char* const names[] = {"Default", "External", "Edid", "Internal", "Null"};
    for (uint64_t i = 0; i < 5; ++i) {
        if (std::strncmp(name40, names[i], 0x40) == 0) {
            *id = i;
            return true;
        }
    }
    return false;
}

// The DisplayName a command carries as its first 0x40 raw bytes.
bool display_from_raw(const ipc::Request& r, uint64_t* id) {
    char nm[0x41] = {};
    if (r.in) std::memcpy(nm, r.in, std::min<size_t>(0x40, r.in_size));
    return display_id_of(nm, id);
}

// The docked and handheld resolutions (amctl::docked). The "Default" display always reports 1080p (Switchbrew Display_services).
void display_size(uint32_t* w, uint32_t* h) {
    *w = amctl::docked() ? 1920 : 1280;
    *h = amctl::docked() ? 1080 : 720;
}

}  // namespace

ViState& vi() {
    static ViState s;
    return s;
}

namespace {

// The display's vsync: a host thread signals the event at 60 Hz. Started by the first GetDisplayVsyncEvent, stopped by vi_nv_reset().
void vsync_main() {
    log_set_thread_name("vi-vsync");
    ViState& s = vi();
    std::unique_lock<std::mutex> lk(s.mu);
    while (!s.vsync_stop) {
        s.cv.wait_for(lk, std::chrono::microseconds(16667), [&] { return s.vsync_stop; });
        if (s.vsync_stop) break;
        s.vsync.signal();
    }
}

}  // namespace

void vsync_stop_and_join() {
    ViState& s = vi();
    std::thread t;
    {
        std::lock_guard<std::mutex> g(s.mu);
        if (!s.vsync_on) return;
        s.vsync_stop = true;
        s.cv.notify_all();
        t = std::move(s.th);
    }
    if (t.joinable()) t.join();
    std::lock_guard<std::mutex> g(s.mu);
    s.vsync_on = false;
    s.vsync_stop = false;
}

namespace {

// Caller holds vi().mu. Layer lookups: a binder id is the slot + 1; nullptr when no layer is open there.
Layer* layer_by_binder(int32_t binder) {
    if (binder < 1 || binder > static_cast<int32_t>(kLayerSlots)) return nullptr;
    Layer& l = vi().layers[binder - 1];
    return l.used ? &l : nullptr;
}

// Caller holds vi().mu. Opens (or finds) the layer; returns its binder id, 0 when every slot is taken.
uint32_t layer_open_locked(uint64_t layer_id) {
    ViState& s = vi();
    int free_slot = -1;
    for (uint32_t i = 0; i < kLayerSlots; ++i) {
        if (s.layers[i].used && s.layers[i].layer_id == layer_id) return i + 1;
        if (!s.layers[i].used && free_slot < 0) free_slot = static_cast<int>(i);
    }
    if (free_slot < 0) return 0;
    Layer& l = s.layers[free_slot];
    display_size(&l.width, &l.height);
    l.used = true;
    l.layer_id = layer_id;
    l.connected = false;
    l.buffer_count = 2;
    l.event = ServiceEvent();
    return static_cast<uint32_t>(free_slot) + 1;
}

// Caller holds vi().mu. CloseLayer and DestroyStrayLayer.
void layer_close_locked(uint64_t layer_id) {
    ViState& s = vi();
    for (uint32_t i = 0; i < kLayerSlots; ++i) {
        Layer& l = s.layers[i];
        if (!l.used || l.layer_id != layer_id) continue;
        l.used = false;
        l.connected = false;
        l.event = ServiceEvent();
        return;
    }
    NXLOG_W(kTagVi, "closing layer %llx, which isn't open", static_cast<unsigned long long>(layer_id));
}

// The native window a layer hands back: a parcel of one flattened binder (Switchbrew Display_services "NativeWindow"): header
// (data size 0x28 at 0, data offset 0x10, objects size 4 at 8, objects offset 0x38 at 0xC), data (0x2 at 0x10, the process word 1
// at 0x14, the binder id as u64 at 0x18, "dispdrv\0" at 0x28), and an empty object list. Returns the size the parcel needs.
uint32_t native_window(const ipc::Buffer& out, uint32_t binder, int64_t* size) {
    if (!binder) return kViOperationFailed;
    uint8_t p[kNativeWindowSize] = {};
    const uint32_t words[] = {0x28, 0x10, 4, 0x38, 2, 1};
    std::memcpy(p, words, sizeof words);
    const uint64_t id = binder;
    std::memcpy(p + 0x18, &id, 8);
    std::memcpy(p + 0x28, "dispdrv", 8);
    *size = static_cast<int64_t>(sizeof p);
    if (out.size < sizeof p) {
        NXLOG_W(kTagVi, "native window buffer of 0x%llx bytes, 0x%x are needed", static_cast<unsigned long long>(out.size),
                static_cast<unsigned>(sizeof p));
        return 0;
    }
    buf_write(out, p, sizeof p);
    return 0;
}

// ---- the binder driver of a layer's buffer queue (IHOSBinderDriver) ----

// A parcel's reader: the header, the interface token (strict policy, then the descriptor as a length and UTF-16 units with a NUL,
// padded to 4), then the call's words.
struct ParcelIn {
    const uint8_t* d = nullptr;
    uint64_t n = 0, pos = 0;
    void open(const uint8_t* data, uint64_t size) {
        d = data;
        n = size;
        uint32_t off = 0;
        if (n >= 8) std::memcpy(&off, d + 4, 4);
        pos = off < n ? off : n;
        u32();                                       // strict policy
        const uint32_t len = u32();
        if (len < 0x100) pos += (((len + 1) * 2) + 3) & ~3u;
        if (pos > n) pos = n;
    }
    uint32_t u32() {
        uint32_t v = 0;
        if (pos + 4 <= n) std::memcpy(&v, d + pos, 4);
        pos += 4;
        return v;
    }
};

struct ParcelOut {
    std::vector<uint8_t> d;
    void u32(uint32_t v) {
        const size_t o = d.size();
        d.resize(o + 4);
        std::memcpy(d.data() + o, &v, 4);
    }
};

// The reply: the header (data size, data offset 16, no objects, objects offset after the data) and the data.
void reply_parcel(const ipc::Buffer& reply, const std::vector<uint8_t>& data) {
    const uint32_t hdr[4] = {static_cast<uint32_t>(data.size()), 16, 0, static_cast<uint32_t>(16 + data.size())};
    std::vector<uint8_t> out(16 + data.size());
    std::memcpy(out.data(), hdr, 16);
    if (!data.empty()) std::memcpy(out.data() + 16, data.data(), data.size());
    if (out.size() > reply.size)
        NXLOG_W(kTagVi, "binder reply of %llu bytes doesn't fit the game's %llu", static_cast<unsigned long long>(out.size()),
                static_cast<unsigned long long>(reply.size));
    buf_write(reply, out.data(), out.size());
}

// One TransactParcel: the layer's buffer queue answers each IGraphicBufferProducer code. The queue never holds a buffer (the game's
// buffers are not presented here), so DequeueBuffer answers -EBUSY and RequestBuffer / QueueBuffer answer NO_INIT. Outputs come
// first and the status word last, except GetBufferHistory, which answers the status and then a count of 0 (Ryujinx).
uint32_t binder_call(int32_t binder, uint32_t code, const ipc::Buffer& parcel, const ipc::Buffer& reply) {
    if (parcel.size < 16) return kViOperationFailed;
    ParcelIn in;
    in.open(buf_ptr(parcel), parcel.size < 0x10000 ? parcel.size : 0x10000);
    ParcelOut out;
    bool history = false;
    uint32_t status = kBqOk;
    {
        ViState& s = vi();
        std::lock_guard<std::mutex> g(s.mu);
        Layer* l = layer_by_binder(binder);
        if (!l) return kViNotFound;
        switch (code) {
        case 1:   // RequestBuffer: a null graphic buffer
            out.u32(0);
            status = kBqNoInit;
            warn_once(code, "RequestBuffer: no buffer in the queue, NO_INIT");
            break;
        case 2: {   // SetBufferCount(i32)
            const int32_t count = static_cast<int32_t>(in.u32());
            if (count < 1) status = kBqBadValue;
            else l->buffer_count = static_cast<uint32_t>(count);
            break;
        }
        case 3:   // DequeueBuffer: no free slot (-EBUSY) and no slot number
            out.u32(static_cast<uint32_t>(-1));
            status = kBqEBusy;
            warn_once(code, "DequeueBuffer: no free buffer, -EBUSY");
            break;
        case 4:
        case 5:
        case 6:
        case 8:
        case 13:
        case 14: break;   // DetachBuffer, DetachNextBuffer, AttachBuffer, CancelBuffer, AllocateBuffers, SetPreallocatedBuffer: accepted
        case 7:   // QueueBuffer: nothing was dequeued
            status = kBqNoInit;
            warn_once(code, "QueueBuffer: nothing dequeued, NO_INIT");
            break;
        case 9: {   // Query(u32 what) -> i32 value
            const uint32_t what = in.u32();
            uint32_t v = 0;
            bool known = true;
            switch (what) {
            case 0: v = l->width; break;                                    // Width
            case 1: v = l->height; break;                                   // Height
            case 2: v = 1; break;                                           // Format: RGBA8888
            case 3: v = 1; break;                                           // MinUnqueuedBuffers
            case 6: v = l->width; break;                                    // DefaultWidth
            case 7: v = l->height; break;                                   // DefaultHeight
            case 9:
            case 10:
            case 11: v = 0; break;                                          // ConsumerRunningBehind, ConsumerUsageBits, StickyTransform
            case 12: v = l->buffer_count; break;                            // MaxBufferCountAsync
            default: known = false; break;
            }
            out.u32(v);
            if (!known) status = kBqBadValue;
            break;
        }
        case 10:   // Connect(bool has_listener, u32 api, bool controlled): QueueBufferOutput {width, height, transform hint, pending}
            l->connected = true;
            out.u32(l->width);
            out.u32(l->height);
            out.u32(0x10);   // NoVSyncCapability, as Ryujinx answers
            out.u32(0);
            break;
        case 11:   // Disconnect(u32 api)
            if (!l->connected) status = kBqNoInit;
            l->connected = false;
            break;
        case 17:   // GetBufferHistory: status, then a count of 0
            history = true;
            break;
        default:
            warn_once(code, "is not implemented: answered with success");
            break;
        }
    }
    if (history) {
        out.u32(status);
        out.u32(0);
    } else {
        out.u32(status);
    }
    reply_parcel(reply, out.d);
    return 0;
}

}  // namespace

// vsync: the first request starts the 60 Hz thread; each request is a new handle onto the one event.
uint32_t vsync_new_handle() {
    ViState& s = vi();
    std::lock_guard<std::mutex> g(s.mu);
    if (!s.vsync_on) {
        s.vsync_on = true;
        s.vsync_stop = false;
        s.th = std::thread(vsync_main);
        NXLOG_I(kTagVi, "display vsync: 60 Hz event started");
    }
    return s.vsync.new_handle();
}

namespace {

uint32_t vsync_handle(ipc::Response& rsp) {
    const uint32_t h = vsync_new_handle();
    if (!h) return kViOperationFailed;
    rsp.out_copy.push_back(h);
    return 0;
}

// An unsignalled event handle (hotplug, mode changed, texture presenting): nothing here raises them.
uint32_t fresh_event_handle(ipc::Response& rsp) {
    ServiceEvent ev;
    const uint32_t h = ev.new_handle();
    if (!h) return kViOperationFailed;
    rsp.out_copy.push_back(h);
    return 0;
}

// The Default display's DisplayInfo (0x60 bytes): name at 0, has-limited-layers at 0x40, layer count max at 0x48, the 1080p size at
// 0x50 and 0x58 (Switchbrew Display_services; the layer count of 1 is ScarletNX's value).
void write_display_info(const ipc::Buffer& out) {
    uint8_t info[0x60] = {};
    std::memcpy(info, "Default", 8);
    info[0x40] = 1;
    const uint64_t one = 1, w = 1920, h = 1080;
    std::memcpy(info + 0x48, &one, 8);
    std::memcpy(info + 0x50, &w, 8);
    std::memcpy(info + 0x58, &h, 8);
    buf_write(out, info, sizeof info);
}

std::shared_ptr<ipc::Service> make_binder_driver();   // forward: the IHOSBinderDriver interface

void setup_binder_driver(ipc::Service& s) {
    s.add_cmd(0, "TransactParcel", [](ipc::Request& r, ipc::Response&) -> uint32_t {
        return binder_call(r.arg<int32_t>(0), r.arg<uint32_t>(4), r.in_buf(0), r.out_buf(0));
    });
    accept_cmd(s, 1, "AdjustRefcount", kTagVi);
    s.add_cmd(2, "GetNativeHandle", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        std::lock_guard<std::mutex> g(vi().mu);
        Layer* l = layer_by_binder(r.arg<int32_t>(0));
        if (!l) return kViNotFound;
        const uint32_t h = l->event.new_handle();
        if (!h) return kViOperationFailed;
        rsp.out_copy.push_back(h);
        return 0;
    });
    s.add_cmd(3, "TransactParcelAuto", [](ipc::Request& r, ipc::Response&) -> uint32_t {
        return binder_call(r.arg<int32_t>(0), r.arg<uint32_t>(4), r.in_buf(0), r.out_buf(0));
    });
}

std::shared_ptr<ipc::Service> make_binder_driver() {
    return make_iface("nns::hosbinder::IHOSBinderDriver", setup_binder_driver);
}

std::shared_ptr<ipc::Service> make_system_display();    // forward
std::shared_ptr<ipc::Service> make_manager_display();   // forward

void setup_system_display(ipc::Service& s) {
    s.add_cmd(1200, "GetZOrderCountMin", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<int64_t>(1);
        return 0;
    });
    s.add_cmd(1202, "GetZOrderCountMax", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<int64_t>(255);
        return 0;
    });
    s.add_cmd(1203, "GetDisplayLogicalResolution", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        uint32_t w = 0, h = 0;
        display_size(&w, &h);
        rsp.push<int32_t>(static_cast<int32_t>(w));
        rsp.push<int32_t>(static_cast<int32_t>(h));
        return 0;
    });
    // DisplayModeInfo: width, height, refresh rate (f32), unknown (u32)
    s.add_cmd(3200, "GetDisplayMode", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        uint32_t w = 0, h = 0;
        display_size(&w, &h);
        rsp.push<uint32_t>(w);
        rsp.push<uint32_t>(h);
        rsp.push<float>(60.0f);
        rsp.push<uint32_t>(0);
        return 0;
    });
}

std::shared_ptr<ipc::Service> make_system_display() {
    return make_iface("nn::visrv::sf::ISystemDisplayService", setup_system_display);
}

void setup_manager_display(ipc::Service& s) {
    s.add_cmd(2010, "CreateManagedLayer", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        uint64_t layer = 0;
        {
            std::lock_guard<std::mutex> g(vi().mu);
            layer = vi().next_stray++;
        }
        rsp.push<uint64_t>(layer);
        return 0;
    });
    s.add_cmd(2300, "AcquireLayerTexturePresentingEvent", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        return fresh_event_handle(rsp);
    });
    s.add_cmd(2302, "GetDisplayHotplugEvent", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        return fresh_event_handle(rsp);
    });
    s.add_cmd(2303, "GetDisplayModeChangedEvent", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        return fresh_event_handle(rsp);
    });
}

std::shared_ptr<ipc::Service> make_manager_display() {
    return make_iface("nn::visrv::sf::IManagerDisplayService", setup_manager_display);
}

void setup_app_display(ipc::Service& s) {
    s.add_cmd(100, "GetRelayService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_binder_driver());
        return 0;
    });
    s.add_cmd(101, "GetSystemDisplayService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_system_display());
        return 0;
    });
    s.add_cmd(102, "GetManagerDisplayService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_manager_display());
        return 0;
    });
    s.add_cmd(103, "GetIndirectDisplayTransactionService", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_binder_driver());
        return 0;
    });
    s.add_cmd(1000, "ListDisplays", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        write_display_info(r.out_buf(0));
        rsp.push<uint64_t>(1);
        return 0;
    });
    s.add_cmd(1010, "OpenDisplay", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        uint64_t id = 0;
        if (!display_from_raw(r, &id)) return kViOperationFailed;
        rsp.push<uint64_t>(id);
        return 0;
    });
    s.add_cmd(1011, "OpenDefaultDisplay", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint64_t>(0);
        return 0;
    });
    accept_cmd(s, 1020, "CloseDisplay", kTagVi);
    accept_cmd(s, 1101, "SetDisplayEnabled", kTagVi);
    s.add_cmd(1102, "GetDisplayResolution", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        uint32_t w = 0, h = 0;
        display_size(&w, &h);
        rsp.push<int64_t>(w);
        rsp.push<int64_t>(h);
        return 0;
    });
    s.add_cmd(2020, "OpenLayer", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        uint64_t display = 0;
        if (!display_from_raw(r, &display)) return kViOperationFailed;
        const uint64_t layer = r.arg<uint64_t>(0x40);   // the AppletResourceUserId that follows is not needed here
        uint32_t binder = 0;
        {
            std::lock_guard<std::mutex> g(vi().mu);
            binder = layer_open_locked(layer);
        }
        int64_t size = 0;
        const uint32_t err = native_window(r.out_buf(0), binder, &size);
        if (err) return err;
        rsp.push<int64_t>(size);
        return 0;
    });
    s.add_cmd(2021, "CloseLayer", [](ipc::Request& r, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> g(vi().mu);
        layer_close_locked(r.arg<uint64_t>(0));
        return 0;
    });
    s.add_cmd(2030, "CreateStrayLayer", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        (void)r.arg<uint32_t>(0);   // layer flags, and the display at 8 (only one display exists)
        uint64_t layer = 0;
        uint32_t binder = 0;
        {
            std::lock_guard<std::mutex> g(vi().mu);
            layer = vi().next_stray++;
            binder = layer_open_locked(layer);
        }
        int64_t size = 0;
        const uint32_t err = native_window(r.out_buf(0), binder, &size);
        if (err) return err;
        rsp.push<uint64_t>(layer);
        rsp.push<int64_t>(size);
        return 0;
    });
    s.add_cmd(2031, "DestroyStrayLayer", [](ipc::Request& r, ipc::Response&) -> uint32_t {
        std::lock_guard<std::mutex> g(vi().mu);
        layer_close_locked(r.arg<uint64_t>(0));
        return 0;
    });
    accept_cmd(s, 2101, "SetLayerScalingMode", kTagVi);
    s.add_cmd(2102, "ConvertScalingMode", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        // nn::vi::ScalingMode (None, Freeze, ScaleToWindow, ScaleAndCrop, PreserveAspectRatio) to Android's values
        static const uint64_t android[] = {3, 0, 1, 2, 4};
        const uint32_t mode = r.arg<uint32_t>(0);
        if (mode >= sizeof android / sizeof android[0]) return kViOperationFailed;
        rsp.push<uint64_t>(android[mode]);
        return 0;
    });
    s.add_cmd(2460, "GetIndirectLayerImageRequiredMemoryInfo", [](ipc::Request& r, ipc::Response& rsp) -> uint32_t {
        const int64_t w = r.arg<int64_t>(0), h = r.arg<int64_t>(8);
        const int64_t block = 0x20000;
        rsp.push<int64_t>((w * h * 4 + block - 1) / block * block);
        rsp.push<int64_t>(0x1000);
        return 0;
    });
    s.add_cmd(5202, "GetDisplayVsyncEvent", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        return vsync_handle(rsp);
    });
    s.add_cmd(5203, "GetDisplayVsyncEventForDebug", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        return vsync_handle(rsp);
    });
}

std::shared_ptr<ipc::Service> make_app_display() {
    return make_iface("nn::visrv::sf::IApplicationDisplayService", setup_app_display);
}

}  // namespace

// vi:u, vi:s and vi:m. GetDisplayService takes a policy word the display ignores; the proxy-name variants take an 8-byte name first.
void setup_vi_u(ipc::Service& s) {
    auto open = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_app_display());
        return 0;
    };
    s.add_cmd(0, "GetDisplayService", open);
    s.add_cmd(1, "GetDisplayServiceWithProxyNameExchange", open);
}

void setup_vi_s(ipc::Service& s) {
    auto open = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_app_display());
        return 0;
    };
    s.add_cmd(1, "GetDisplayService", open);
    s.add_cmd(3, "GetDisplayServiceWithProxyNameExchange", open);
}

void setup_vi_m(ipc::Service& s) {
    auto open = [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push_object(make_app_display());
        return 0;
    };
    s.add_cmd(2, "GetDisplayService", open);
    s.add_cmd(3, "GetDisplayServiceWithProxyNameExchange", open);
}

}  // namespace nxrt::services::vinv
