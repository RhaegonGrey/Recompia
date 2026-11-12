// SPDX-License-Identifier: GPL-3.0-or-later
// Internal to the small services of misc*.cpp: the result codes, the accept/refuse command helpers, the process-wide event and clock
// state misc_reset() clears, and the service factories register_misc() registers.
#pragma once
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "nxrt/log.h"
#include "services_internal.h"

namespace nxrt::services::miscsvc {

using ipc::Handler;
using ipc::Request;
using ipc::Response;
using ipc::Service;

// ---- result codes ---------------------------------------------------------------------------------------------------------------
// ro NotLoaded (module 22, desc 1028 = 0x80816): Switchbrew Error_codes.
constexpr uint32_t kRoNotLoaded = res(22, 1028);
// Sf_Hipc NotSupported (module 11, desc 1 = 0x20B): Switchbrew Error_codes. Used where the real console's code is unknown.
constexpr uint32_t kNotSupported = res(11, 1);
// No device: ScarletNX misc.c NFP_DEVICE_NOT_FOUND / NFC_DEVICE_NOT_FOUND (modules 115 Nfp and 127 Nfc, names from Switchbrew). Not verified
// against Switchbrew's listing, which stops before those modules.
constexpr uint32_t kNfpNoDevice = res(115, 64);
constexpr uint32_t kNfcNoDevice = res(127, 64);
constexpr uint32_t kOlscNullArgument = res(179, 100);   // Ryubing Olsc/ResultCode.cs
constexpr uint32_t kOlscNotInitialized = res(179, 101);
// Mii: ScarletNX misc.c MII_NOT_FOUND / MII_INVALID_ARGUMENT (module 126 Mii). Not verified against Switchbrew.
constexpr uint32_t kMiiNotFound = res(126, 4);
constexpr uint32_t kMiiInvalidArgument = res(126, 1);
// aoc: ScarletNX misc.c AOC_NO_PURCHASED_PRODUCT (module 164 = Ec, per Switchbrew) and AOC_NOT_CONNECTED (module 110 = Nifm).
constexpr uint32_t kAocNoPurchase = res(164, 400);
constexpr uint32_t kAocNotConnected = res(110, 300);

// Sizes
constexpr uint64_t kCharInfoSize = 0x58;          // nn::mii::CharInfo (ScarletNX misc.swipc)
constexpr uint64_t kAocBaseIdOffset = 0x1000;     // add-on content base id = application id + 0x1000
constexpr uint32_t kOpusWorkBufferSize = 0x8000;  // fixed size reported by GetWorkBufferSize* (page aligned, as Switchbrew says)
constexpr uint64_t kIrsSharedMemorySize = 0x1000; // size of the IR sensor shared memory handed out by GetIrsensorSharedMemoryHandle
constexpr size_t kMmSlots = 16;                    // clock requests kept at once (ScarletNX misc.c keeps a fixed table too)

// A command that is accepted and ignored (logged at Debug).
Handler accept(const char* tag, const char* what);
// A command that answers with an error result (logged at Warn).
Handler refuse(uint32_t rc, const char* tag, const char* what);
void zero_buffer(const ipc::Buffer& b);

// Events held for the process (signalled never: none of these services has an event to raise).
struct MiscEvents {
    ServiceEvent aoc_list_changed;
    ServiceEvent aoc_purchased;
    ServiceEvent nfc_available, nfc_activate, nfc_deactivate;
    ServiceEvent nfp_available, nfp_activate, nfp_deactivate;
};
MiscEvents& events();

// ---- mm:u -------------------------------------------------------------------------------------------------------------------------
// Multimedia clock requests. The old forms (0 to 3) name the module; the new ones (4 to 7) return a request id. The rate is kept per
// request and read back. Nothing is clocked here.
struct MmSlot {
    bool used = false;
    bool by_module = false;   // the old forms key the slot by module, the new ones by request id
    uint32_t key = 0;
    uint32_t module = 0;
    uint32_t current = 0;
};
struct MmState {
    std::mutex mu;
    uint32_t next_id = 0;
    MmSlot slots[kMmSlots];
};
MmState& mm_state();

// ---- pcm and banana ---------------------------------------------------------------------------------------------------------------
// pcm: the interface has no command table in the sources read (Ryubing's IManager is empty; Switchbrew lists pcm as not present on
// retail), so every command reaches the framework's default and answers kStubService.
// banana: the profiler service, not on retail. Every command is logged at Warn and answered with kStubService.
class BananaService : public ipc::StubService {
public:
    BananaService() : ipc::StubService("banana") {}
    uint32_t handle(Request& req, Response& rsp) override {
        NXLOG_W("banana", "command %u (msg type %u) asked of banana; answering StubService", req.cmd, req.msg_type);
        return ipc::StubService::handle(req, rsp);
    }
};

// ---- htc, htc:tenv, htcs ----------------------------------------------------------------------------------------------------------
// TMA services: absent on retail (Switchbrew). Every command answers NotSupported and is logged.
using CmdNames = std::vector<std::pair<uint32_t, const char*>>;

// Service factories, one per misc_*.cpp; register_misc() (misc.cpp) registers them.
std::shared_ptr<Service> make_csrng();
std::shared_ptr<Service> make_ro();
std::shared_ptr<Service> make_mm();
std::shared_ptr<Service> make_hwopus();
std::shared_ptr<Service> make_aoc();
std::shared_ptr<Service> make_album_app();
std::shared_ptr<Service> make_mii_static();
std::shared_ptr<Service> make_irs();
std::shared_ptr<Service> make_ectx();
std::shared_ptr<Service> make_olsc();
std::shared_ptr<Service> make_nfc_user();
std::shared_ptr<Service> make_nfp_user();
std::shared_ptr<Service> make_mifare_user();
std::shared_ptr<Service> make_user_manager(const char* name, std::shared_ptr<Service> (*make_user)());
std::shared_ptr<Service> make_tma(const char* name, const CmdNames& cmds);
CmdNames htc_cmds();
CmdNames htcs_cmds();
CmdNames htc_tenv_cmds();
std::shared_ptr<ipc::Service> make_streamplay_user();

}  // namespace nxrt::services::miscsvc
