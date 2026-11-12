// SPDX-License-Identifier: GPL-3.0-or-later
// time:u, time:a, time:s, time:r (S3c): the standard clocks, the steady clock, the time zone service and the time shared memory.
// Adapted from ryubing (Ryujinx, MIT) src/Ryujinx.HLE/HOS/Services/Time: TimeSharedMemory.cs (shared memory offsets, paddings and the
// lifo index rule), StaticService/IStaticServiceForGlue.cs (IStaticService command ids and the time:u/a/r split), StaticService/
// ISystemClock.cs, ISteadyClock.cs, ITimeZoneServiceForGlue.cs and ITimeZoneServiceForPsc.cs (command ids and output layouts),
// Types/TimePermissions.cs (the write masks per flavour). ScarletNX src/nx/hle/services/time.swipc (GPL-3.0-or-later) was used to
// cross-check the command ids and struct sizes. The Switchbrew "Services API" page was used for the list of time services.
//
// Model (one process-wide state, guarded by one mutex):
//   * posix time of a clock = host UTC (GetSystemTimePreciseAsFileTime) + that clock's offset. The user and the local clock share the
//     local offset (on the console the user clock is the local clock); the network and the ephemeral network clock have their own.
//   * steady clock: ticks since start-up (clock::ticks, 31.25 MHz on Switch 2, 1 tick = 32 ns) + the base + the internal offset + the
//     test offset (both in ns, TimeSpanType). The base is the host RTC value at start-up (seconds since 2000-01-01, as GetRtcValue),
//     so the steady clock carries on across runs like the console's, which is RTC-backed and survives reboots: a SteadyClockTimePoint
//     the game saved in an earlier run still measures real elapsed time. Seconds are the floor of the sum; the sequence is monotonic.
//   * every change of an offset rewrites the shared memory, so the guest's fast GetCurrentTime path sees the same values.
//   * host sleep ([time] resync, PR #172): the host counter behind clock::ticks stops while the host sleeps under Proton
//     (CLOCK_MONOTONIC), so with the game open across a sleep every clock fell behind by the nap until the next launch. A watcher
//     compares host UTC with the steady clock every 2 s; when the gap grew by more than 2 s the steady clock gets the difference
//     (sleep_ns: the console's RTC-backed steady clock runs on through sleep) and the shared memory is written again. A host clock
//     set back only rewrites the contexts: the steady clock never runs backwards.
//
// Time shared memory (0x1000 bytes, offsets as TimeSharedMemory.cs writes them). A block is a u32 index at `off`, `pad` bytes, then two
// slots; the live slot is (index & 1). Writers bump the index after writing the free slot.
//   0x000 SteadyClockContext x2 (pad 4; slots 0x008, 0x020; 0x18 each): u64 internal_offset (= steady time - raw tick time, ns), uuid
//   0x038 Local SystemClockContext x2 (pad 4; slots 0x040, 0x060; 0x20 each): i64 offset, {i64 time_point, uuid}
//   0x080 Network SystemClockContext x2 (pad 4; slots 0x088, 0x0A8)
//   0x0C8 automatic correction flag x2 (pad 0; slots 0x0CC, 0x0CD; 1 byte each)
//   0x0D0 ContinuousAdjustmentTimePoint x2 (pad 4; slots 0x0D8, 0x110; 0x38 each), see Adjust below.
// The user clock has no slot of its own (the layout of the console has none): it is the local context.
//
// Which block the game reads: Pokopia's SDK (nn::time::detail::SharedMemoryClient in the sdk module) does NOT read the steady
// context at 0x000 any more. GetStandardSteadyClockCurrentTimePoint, CalculateSystemClockCurrentTime and ...CurrentTimespec all take
// the steady time from the ContinuousAdjustmentTimePoint (disassembly, sdk +0x557160, +0x5572c0, +0x5576c0):
//     raw  = ConvertToTimeSpan(GetSystemTick())                  (ns since host boot here: QueryPerformanceCounter)
//     adj  = base_offset + (((raw - clock_offset) * multiplier) >> divisor_log2)
//     adj  = multiplier < 0 ? max(adj, limit_offset) : min(adj, limit_offset)
//     steady = (raw + adj) / 1e9 s,  posix = context.offset + steady   (context and adjustment uuids must match)
// i.e. the adjustment slews from base_offset towards limit_offset. Ryubing's layout names (and our old values: multiplier 1,
// base 0, limit 0) gave adj = 0, so the game's steady clock was the raw host counter: every local time it showed was off by
// (host counter at start-up - the runtime's steady time), +29 h on a handheld up for a day, -52 min on one rebooted 8 min before.
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "nxrt/clock.h"
#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/text.h"
#include "services_internal.h"
#include "timezone.h"

namespace nxrt::services {
namespace {

// ---- constants ----------------------------------------------------------------------------------------------------------------
constexpr int64_t kMinSteadyBaseSeconds = 3600;                         // floor of the steady base (a host clock set before 2000)
// clock_source_id. "clk1" was a steady clock that restarted at 3600 s every run (and the game read the raw host counter instead):
// time points saved under it measure nothing. The RTC-based clock gets a new id, so the game sees those as another clock's (as after
// an RTC reset on the console) instead of computing spans against them.
constexpr uint8_t kSourceId[16] = {'n', 'x', 'r', 't', '-', 's', 't', 'e', 'a', 'd', 'y', '-', 'c', 'l', 'k', '2'};
constexpr uint32_t kShmSize = 0x1000;
constexpr int64_t kRtcEpochUnix = 946684800;                            // 2000-01-01T00:00:00Z in posix seconds (RTC counts from here)

constexpr uint32_t kOffSteady = 0x000, kPadSteady = 4;
constexpr uint32_t kOffLocal = 0x038, kPadLocal = 4;
constexpr uint32_t kOffNetwork = 0x080, kPadNetwork = 4;
constexpr uint32_t kOffAutoCorr = 0x0C8, kPadAutoCorr = 0;
constexpr uint32_t kOffAdjust = 0x0D0, kPadAdjust = 4;

// Write masks (Ryujinx TimePermissions): what a flavour may change.
enum : uint32_t {
    kWriteLocal = 0x1,
    kWriteUser = 0x2,
    kWriteNetwork = 0x4,
    kWriteTimeZone = 0x8,
    kWriteSteady = 0x10,
};

enum class Clock { User, Local, Network, Ephemeral };

// Horizon time-module results (module 116, description in bits 9 and up): Ryujinx ResultCode.
uint32_t err_perm() { return res(116, 1); }                 // PermissionDenied
uint32_t err_out_of_range() { return res(116, 902); }       // OutOfRange: a buffer of the wrong size
uint32_t err_tz_conversion() { return res(116, 903); }      // TimeZoneConversionFailed: missing or invalid rule
uint32_t err_tz_not_found() { return res(116, 989); }       // TimeZoneNotFound: a location name we cannot load

// ---- structs written into the shared memory (sizes asserted) ----------------------------------------------------------------
struct SteadyCtx {
    uint64_t internal_offset;
    uint8_t source[16];
};
struct SysCtx {
    int64_t offset;
    int64_t time_point;
    uint8_t source[16];
};
// nn::time ContinuousAdjustmentTimePoint as Pokopia's SDK reads it (see the top of the file). Ryubing declares the last 0x20 bytes
// as a SystemClockContext; the SDK reads them as two adjustment bounds and the uuid.
struct Adjust {
    uint64_t clock_offset;     // raw tick time (ns) where the slew starts
    int64_t multiplier;        // slew rate numerator (0: a fixed offset)
    int64_t divisor_log2;      // slew rate denominator, as a shift
    int64_t base_offset;       // steady - raw (ns) at clock_offset
    int64_t limit_offset;      // steady - raw (ns) the slew stops at
    uint8_t source[16];
};
static_assert(sizeof(SteadyCtx) == 0x18);
static_assert(sizeof(SysCtx) == 0x20);
static_assert(sizeof(Adjust) == 0x38);

// ---- state --------------------------------------------------------------------------------------------------------------------
struct State {
    bool created = false;
    uint64_t tick0_ns = 0;                                // raw tick at start-up, in ns (the ContinuousAdjustment clock_offset)
    int64_t base_ns = 0;                                  // steady time at start-up: the host RTC value then, in ns
    int64_t internal_ns = 0, test_ns = 0;                 // steady clock offsets (TimeSpanType, ns)
    int64_t sleep_ns = 0;                                 // host sleep the counter missed, added to the steady clock ([time] resync)
    int64_t gap_ns = 0;                                   // host UTC - steady time (ns) when the clocks were last in step
    int64_t local_delta = 0, network_delta = 0, ephemeral_delta = 0;   // seconds added to host UTC per clock
    bool auto_correction = false;
    bool scope_all = false;                               // [time] clock_scope = all: the network clocks follow the game clock's offset
    bool device_utc = false;                              // [time] zone = UTC
    bool hosts_ready = false;
    std::string host_name;
    std::vector<uint8_t> host_rule, utc_rule;
    int64_t location_updated = 0;                         // steady seconds when the device location was last set
    std::unique_ptr<ServiceSharedMemory> shm;
    ServiceEvent clock_event, tz_event;                   // never signaled
};

std::mutex& mu() {
    static std::mutex m;
    return m;
}
State& raw_state() {
    static State s;
    return s;
}

std::string str_of(const char* p, size_t n) {
    size_t l = 0;
    while (l < n && p[l]) ++l;
    return std::string(p, l);
}

// ---- clocks -------------------------------------------------------------------------------------------------------------------
// Console ticks to ns (19.2 MHz: 625 / 12 ns per tick; 31.25 MHz: 32 ns per tick).
uint64_t raw_ticks_ns() { return clock::now_ns(); }

int64_t host_unix_now() {
#if defined(_WIN32)
    const uint64_t t = clock::wall_filetime();   // 100 ns ticks since 1601-01-01 (the host clock; a fixed epoch + virtual time in deterministic test mode)
    return static_cast<int64_t>((t - 116444736000000000ull) / 10000000ull);
#else
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
#endif
}

// Host UTC in ns since 1970 (the watcher needs better than seconds).
int64_t host_unix_now_ns() {
#if defined(_WIN32)
    const uint64_t t = clock::wall_filetime();
    return static_cast<int64_t>(t - 116444736000000000ull) * 100;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
#endif
}

// Steady time minus raw tick time (ns): what the guest adds to the raw tick to read the steady clock. Negative once the host counter
// is past the steady time; the SDK adds it as a signed value.
int64_t steady_minus_raw_ns(const State& s) {
    return s.base_ns + s.internal_ns + s.test_ns + s.sleep_ns - static_cast<int64_t>(s.tick0_ns);
}

int64_t steady_total_ns(const State& s) { return static_cast<int64_t>(raw_ticks_ns()) + steady_minus_raw_ns(s); }
int64_t steady_secs(const State& s) { return steady_total_ns(s) / 1000000000; }
// The steady clock without the offsets the game or tests set (internal, test): what the host counter and the host sleep alone give.
int64_t host_steady_ns(const State& s) {
    return static_cast<int64_t>(raw_ticks_ns()) + s.base_ns + s.sleep_ns - static_cast<int64_t>(s.tick0_ns);
}

struct Now {
    int64_t host, steady;
};
Now now_of(const State& s) {
    Now n{};
    n.host = host_unix_now();
    n.steady = steady_secs(s);
    return n;
}

int64_t& delta_of(State& s, Clock c) {
    switch (c) {
        case Clock::Network: return s.network_delta;
        case Clock::Ephemeral: return s.ephemeral_delta;
        default: return s.local_delta;   // user and local share the local offset
    }
}
uint32_t write_bit_of(Clock c) {
    switch (c) {
        case Clock::User: return kWriteUser;
        case Clock::Local: return kWriteLocal;
        default: return kWriteNetwork;   // network and ephemeral network
    }
}

SysCtx sys_ctx(const Now& n, int64_t delta) {
    SysCtx c{};
    c.offset = n.host + delta - n.steady;
    c.time_point = n.steady;
    std::memcpy(c.source, kSourceId, 16);
    return c;
}

// ---- time zone rules ----------------------------------------------------------------------------------------------------------
void ensure_hosts(State& s) {
    if (s.hosts_ready) return;
    s.host_rule.assign(tz::kRuleSize, 0);
    s.utc_rule.assign(tz::kRuleSize, 0);
    tz::make_utc_rule(s.utc_rule.data());
    std::string name;
    const bool built = tz::make_host_rule(s.host_rule.data(), &name);
    s.host_name = name.empty() ? std::string("UTC") : name;
    s.hosts_ready = true;
    tz::CalendarTime c{};
    tz::CalendarAdditionalInfo i{};
    const bool ok = tz::to_calendar(s.host_rule.data(), static_cast<int64_t>(std::time(nullptr)), &c, &i);
    NXLOG_I("time", "host time zone %s (%s): now %04d-%02d-%02d %02d:%02d %.8s, UTC%+d s%s", s.host_name.c_str(),
            built ? "rule built" : "no host rule, using UTC", c.year, c.month, c.day, c.hour, c.minute, i.tz_name, i.utc_offset,
            ok ? "" : " (conversion failed)");
}
const uint8_t* device_rule(State& s) {
    ensure_hosts(s);
    return s.device_utc ? s.utc_rule.data() : s.host_rule.data();
}
std::string device_name(State& s) {
    ensure_hosts(s);
    return s.device_utc ? std::string("UTC") : s.host_name;
}
// The location names this runtime can load: "UTC" and the host zone. No tz database is shipped, so these are the only two.
std::vector<std::string> location_names(State& s) {
    ensure_hosts(s);
    std::vector<std::string> v{"UTC"};
    if (s.host_name != "UTC") v.push_back(s.host_name);
    return v;
}
const std::vector<uint8_t>* rule_for_name(State& s, const std::string& name) {
    ensure_hosts(s);
    if (name == "UTC") return &s.utc_rule;
    if (name == s.host_name) return &s.host_rule;
    return nullptr;
}

// ---- shared memory ------------------------------------------------------------------------------------------------------------
uint8_t* shm_base(State& s) { return (s.shm && s.shm->ok()) ? reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(s.shm->host())) : nullptr; }

// Ryujinx TimeSharedMemory.WriteObjectToSharedMemory: write the free slot, then publish the new index.
void lifo_write(uint8_t* base, uint32_t off, uint32_t pad, const void* obj, uint32_t size) {
    uint32_t idx = 0;
    std::memcpy(&idx, base + off, 4);
    const uint32_t n = idx + 1;
    std::memcpy(base + off + 4 + pad + (n & 1) * size, obj, size);
    std::atomic_thread_fence(std::memory_order_seq_cst);
    std::memcpy(base + off, &n, 4);
}

void write_shm(State& s) {
    uint8_t* b = shm_base(s);
    if (!b) return;
    const Now n = now_of(s);

    const int64_t steady_minus_raw = steady_minus_raw_ns(s);

    // Older SDKs read the steady clock as internal_offset + the raw tick in ns (Ryubing TimeSharedMemory.SetupStandardSteadyClock).
    SteadyCtx sc{};
    sc.internal_offset = static_cast<uint64_t>(steady_minus_raw);
    std::memcpy(sc.source, kSourceId, 16);
    lifo_write(b, kOffSteady, kPadSteady, &sc, sizeof sc);

    const SysCtx lc = sys_ctx(n, s.local_delta);
    lifo_write(b, kOffLocal, kPadLocal, &lc, sizeof lc);
    const SysCtx nc = sys_ctx(n, s.network_delta);
    lifo_write(b, kOffNetwork, kPadNetwork, &nc, sizeof nc);

    const uint8_t ac = s.auto_correction ? 1 : 0;
    lifo_write(b, kOffAutoCorr, kPadAutoCorr, &ac, 1);

    // Pokopia's SDK reads the steady clock here (top of the file): no slew, the offset fixed at steady - raw from the start tick on, so
    // the guest's steady time is the runtime's and context.offset + it is the clock's posix time.
    Adjust adj{};
    adj.clock_offset = s.tick0_ns;
    adj.multiplier = 0;
    adj.divisor_log2 = 0;
    adj.base_offset = steady_minus_raw;
    adj.limit_offset = steady_minus_raw;
    std::memcpy(adj.source, kSourceId, 16);
    lifo_write(b, kOffAdjust, kPadAdjust, &adj, sizeof adj);
}

// Start-up state: offsets zero except the configured local offset, the start tick taken now, the shared memory zeroed and written once.
void init_locked(State& s) {
    s.tick0_ns = raw_ticks_ns();
    const int64_t rtc = host_unix_now() - kRtcEpochUnix;
    s.base_ns = (rtc > kMinSteadyBaseSeconds ? rtc : kMinSteadyBaseSeconds) * 1000000000ll;
    s.internal_ns = 0;
    s.test_ns = 0;
    s.sleep_ns = 0;
    // [time] clock_offset (minutes, default 0; issue #203, set from the launcher's Settings, Game clock): the user and local clock
    // run that far ahead of the host clock (behind when negative), as when the console's date and time are set by hand. The game
    // sees that through its clock snapshots like a change the player made (CalculateStandardUserSystemClockDifferenceByUser); the
    // steady and network clocks stay on host time, as on the console. Limited to +-10 years.
    constexpr int64_t kMaxOffsetMinutes = 10ll * 366 * 24 * 60;
    const int64_t offset_min = config_get_int("time", "clock_offset", 0);
    s.local_delta = (offset_min < -kMaxOffsetMinutes ? -kMaxOffsetMinutes : offset_min > kMaxOffsetMinutes ? kMaxOffsetMinutes : offset_min) * 60;
    s.network_delta = 0;
    s.ephemeral_delta = 0;
    s.scope_all = false;
    // [time] clock_scope (test key, default "user"): "all" moves the network and ephemeral network clocks by the same offset, for
    // game code that prefers the network clock. Anything else is "user" (the behaviour above).
    {
        const std::string scope = text::lower(config_get_str("time", "clock_scope", "user"));
        if (scope != "user" && scope != "all")
            NXLOG_W("time", "[time] clock_scope = %s is not user or all; using user", scope.c_str());
        else if (scope == "all") {
            s.scope_all = true;
            NXLOG_I("time", "time: [time] clock_scope = all: the network clocks are moved by the same offset as the game clock");
            if (s.local_delta) {
                s.network_delta = s.local_delta;
                s.ephemeral_delta = s.local_delta;
            }
        }
    }
    s.auto_correction = false;
    s.device_utc = text::lower(config_get_str("time", "zone", "host")) == "utc";
    s.hosts_ready = false;
    s.host_name.clear();
    s.host_rule.clear();
    s.utc_rule.clear();
    if (!s.shm) s.shm = std::make_unique<ServiceSharedMemory>(kShmSize, "time");
    s.location_updated = steady_secs(s);
    if (uint8_t* b = shm_base(s)) {
        std::memset(b, 0, kShmSize);
        write_shm(s);
    }
    s.gap_ns = host_unix_now_ns() - host_steady_ns(s);
    NXLOG_I("time", "steady clock: %lld s at start-up (host RTC base), host counter %llu s, the game adds %lld ns to it",
            static_cast<long long>(steady_secs(s)), static_cast<unsigned long long>(s.tick0_ns / 1000000000ull),
            static_cast<long long>(steady_minus_raw_ns(s)));
    if (s.local_delta)
        NXLOG_I("time", "game clock: %+lld min from the host clock ([time] clock_offset)", static_cast<long long>(s.local_delta / 60));
}

// [time] resync: one check, under mu(). Host UTC - steady time stays fixed while both run; it grows when the host slept (the counter
// stopped) or its clock was set forward, and shrinks when the clock was set back.
constexpr int64_t kResyncNs = 2000000000;   // drift that counts as a sleep or a clock change; under it is jitter
void resync_locked(State& s) {
    const int64_t gap = host_unix_now_ns() - host_steady_ns(s);
    const int64_t d = gap - s.gap_ns;
    if (d > -kResyncNs && d < kResyncNs) return;
    if (d > 0) s.sleep_ns += d;   // slept (or the clock moved forward): the steady clock catches up, like the console's after sleep
    write_shm(s);
    s.gap_ns = host_unix_now_ns() - host_steady_ns(s);
    NXLOG_I("time", "host clock %s %lld s against the steady clock (%s): clocks written again",
            d > 0 ? "ahead by" : "behind by", static_cast<long long>((d > 0 ? d : -d) / 1000000000),
            d > 0 ? "host sleep or clock moved forward; steady clock moved forward" : "host clock set back; steady clock kept");
}

void start_resync_watcher() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (!config_get_bool("time", "resync", true)) {
            NXLOG_I("time", "resync off: the clocks do not follow a host sleep until the next launch");
            return;
        }
        std::thread([] {
            log_set_thread_name("time-resync");
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                std::lock_guard<std::mutex> lk(mu());
                State& s = raw_state();
                if (s.created) resync_locked(s);
            }
        }).detach();
    });
}

// Lazily brings the state up (the shared memory needs the kernel region, so nothing is allocated before the first service use).
State& state_locked() {
    State& s = raw_state();
    if (!s.created) {
        s.created = true;
        init_locked(s);
        start_resync_watcher();
    }
    return s;
}

// nn::time::sf::ClockSnapshot (0xD0 bytes): Ryubing Time/Clock/Types/ClockSnapshot.cs.
struct ClockSnapshot {
    SysCtx user_ctx, network_ctx;                 // 0x00, 0x20
    int64_t user_time, network_time;              // 0x40, 0x48
    tz::CalendarTime user_cal, network_cal;       // 0x50, 0x58
    tz::CalendarAdditionalInfo user_info, network_info;   // 0x60, 0x78
    int64_t steady_time_point;                    // 0x90: SteadyClockTimePoint {time_point, clock source}
    uint8_t steady_source[16];
    char location[0x24];                          // 0xA8
    uint8_t auto_correction;                      // 0xCC
    uint8_t type;                                 // 0xCD
    uint16_t unknown;
};
static_assert(sizeof(ClockSnapshot) == 0xD0);

// Ryubing IStaticServiceForPsc.GetClockSnapshotFromSystemClockContextInternal: the steady time point now, both contexts, the times
// they give at that point (a context from another clock source gives none: the user one fails, the network one is 0) and the
// calendar times of both under the device rule. Called with mu() held.
uint32_t make_snapshot(State& st, const SysCtx& user, const SysCtx& network, uint8_t type, ClockSnapshot* out) {
    ClockSnapshot cs{};
    const int64_t steady = steady_secs(st);
    cs.auto_correction = st.auto_correction ? 1 : 0;
    cs.user_ctx = user;
    cs.network_ctx = network;
    cs.steady_time_point = steady;
    std::memcpy(cs.steady_source, kSourceId, 16);
    const std::string name = device_name(st);
    std::memcpy(cs.location, name.data(), name.size() < sizeof cs.location - 1 ? name.size() : sizeof cs.location - 1);
    if (std::memcmp(user.source, kSourceId, 16) != 0) return res(116, 102);   // ClockSnapshot.GetCurrentTime: TimeMismatch
    cs.user_time = user.offset + steady;
    if (!tz::to_calendar(device_rule(st), cs.user_time, &cs.user_cal, &cs.user_info)) return err_tz_conversion();
    cs.network_time = std::memcmp(network.source, kSourceId, 16) == 0 ? network.offset + steady : 0;
    if (!tz::to_calendar(device_rule(st), cs.network_time, &cs.network_cal, &cs.network_info)) return err_tz_conversion();
    cs.type = type;
    *out = cs;
    return 0;
}

void push_uuid(ipc::Response& rsp) { rsp.out.insert(rsp.out.end(), kSourceId, kSourceId + 16); }

// Registers a command and logs its first 50 calls at Info (service object, command id and name; no arguments).
void tcmd(ipc::Service& s, uint32_t id, const char* name, ipc::Handler h) {
    const std::string iface = s.name();
    auto calls = std::make_shared<std::atomic<int>>(0);
    s.add_cmd(id, name, [iface, id, name, calls, h = std::move(h)](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        if (calls->load(std::memory_order_relaxed) <= 50) {
            const int n = ++*calls;
            if (n <= 50) NXLOG_I("time", "ask: %s cmd %u %s (call %d)", iface.c_str(), id, name, n);
        }
        return h(req, rsp);
    });
}

// ---- service objects ----------------------------------------------------------------------------------------------------------
std::shared_ptr<ipc::Service> make_clock(Clock kind, uint32_t perm) {
    return make_iface("ISystemClock", [kind, perm](ipc::Service& s) {
        tcmd(s, 0, "GetCurrentTime", [kind](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            const Now n = now_of(st);
            rsp.push<int64_t>(n.host + delta_of(st, kind));
            return 0;
        });
        tcmd(s, 1, "SetCurrentTime", [kind, perm](ipc::Request& req, ipc::Response&) -> uint32_t {
            if (!(perm & write_bit_of(kind))) return err_perm();
            const int64_t posix = req.arg<int64_t>(0);
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            const Now n = now_of(st);
            delta_of(st, kind) = posix - n.host;
            write_shm(st);
            return 0;
        });
        tcmd(s, 2, "GetSystemClockContext", [kind](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            const SysCtx c = sys_ctx(now_of(st), delta_of(st, kind));
            rsp.push(c);
            return 0;
        });
        tcmd(s, 4, "GetOperationEventReadableHandle", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            rsp.out_copy.push_back(st.clock_event.new_handle());
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_steady(uint32_t perm) {
    return make_iface("ISteadyClock", [perm](ipc::Service& s) {
        tcmd(s, 0, "GetCurrentTimePoint", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            rsp.push<int64_t>(steady_secs(st));
            push_uuid(rsp);
            return 0;
        });
        tcmd(s, 2, "GetTestOffset", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<int64_t>(state_locked().test_ns);
            return 0;
        });
        tcmd(s, 3, "SetTestOffset", [perm](ipc::Request& req, ipc::Response&) -> uint32_t {
            if (!(perm & kWriteSteady)) return err_perm();
            const int64_t v = req.arg<int64_t>(0);
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            st.test_ns = v;
            write_shm(st);
            return 0;
        });
        tcmd(s, 100, "GetRtcValue", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            // Seconds since 2000-01-01 of the host clock (the console keeps the RTC the same way; no RTC hardware here).
            const int64_t host = host_unix_now();
            rsp.push<uint64_t>(static_cast<uint64_t>(host - kRtcEpochUnix));
            return 0;
        });
        tcmd(s, 101, "IsRtcResetDetected", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint8_t>(0);
            return 0;
        });
        tcmd(s, 102, "GetSetupResultValue", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push<uint32_t>(0);
            return 0;
        });
        tcmd(s, 200, "GetInternalOffset", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<int64_t>(state_locked().internal_ns);
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_tz(uint32_t perm) {
    return make_iface("ITimeZoneService", [perm](ipc::Service& s) {
        tcmd(s, 0, "GetDeviceLocationName", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            char name[0x24] = {};
            const std::string n = device_name(st);
            std::memcpy(name, n.data(), n.size() < sizeof name - 1 ? n.size() : sizeof name - 1);
            rsp.out.insert(rsp.out.end(), name, name + sizeof name);
            return 0;
        });
        tcmd(s, 1, "SetDeviceLocationName", [perm](ipc::Request& req, ipc::Response&) -> uint32_t {
            if (!(perm & kWriteTimeZone)) return err_perm();
            const auto raw = req.arg<std::array<char, 0x24>>(0);
            const std::string name = str_of(raw.data(), raw.size());
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            ensure_hosts(st);
            if (name == "UTC") {
                st.device_utc = true;
            } else if (name == st.host_name) {
                st.device_utc = false;
            } else {
                return err_tz_not_found();
            }
            st.location_updated = steady_secs(st);
            return 0;
        });
        tcmd(s, 2, "GetTotalLocationNameCount", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<int32_t>(static_cast<int32_t>(location_names(state_locked()).size()));
            return 0;
        });
        tcmd(s, 3, "LoadLocationNameList", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const uint32_t index = req.arg<uint32_t>(0);
            const ipc::Buffer out = req.out_buf(0);
            std::lock_guard<std::mutex> lk(mu());
            const std::vector<std::string> names = location_names(state_locked());
            const size_t cap = out.size / 0x24 < 6 ? static_cast<size_t>(out.size / 0x24) : 6;
            std::vector<char> data;
            uint32_t count = 0;
            for (size_t i = index; i < names.size() && count < cap; ++i, ++count) {
                char entry[0x24] = {};
                std::memcpy(entry, names[i].data(), names[i].size() < sizeof entry - 1 ? names[i].size() : sizeof entry - 1);
                data.insert(data.end(), entry, entry + sizeof entry);
            }
            if (!data.empty()) buf_write(out, data.data(), data.size());
            rsp.push<uint32_t>(count);
            return 0;
        });
        tcmd(s, 4, "LoadTimeZoneRule", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const auto raw = req.arg<std::array<char, 0x24>>(0);
            const ipc::Buffer out = req.out_buf(0);
            if (out.size != tz::kRuleSize) return err_out_of_range();
            const std::string name = str_of(raw.data(), raw.size());
            std::lock_guard<std::mutex> lk(mu());
            const std::vector<uint8_t>* rule = rule_for_name(state_locked(), name);
            if (!rule) return err_tz_not_found();
            buf_write(out, rule->data(), tz::kRuleSize);
            return 0;
        });
        tcmd(s, 5, "GetTimeZoneRuleVersion", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            // No tz database is shipped, so there is no rule version to report: 16 zero bytes.
            rsp.out.insert(rsp.out.end(), 0x10, uint8_t{0});
            return 0;
        });
        tcmd(s, 6, "GetDeviceLocationNameAndUpdatedTime", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            char name[0x24] = {};
            const std::string n = device_name(st);
            std::memcpy(name, n.data(), n.size() < sizeof name - 1 ? n.size() : sizeof name - 1);
            rsp.out.insert(rsp.out.end(), name, name + sizeof name);
            rsp.out.insert(rsp.out.end(), 4, uint8_t{0});          // padding before the 8-aligned time point
            rsp.push<int64_t>(st.location_updated);
            push_uuid(rsp);
            return 0;
        });
        tcmd(s, 20, "GetDeviceLocationNameOperationEventReadableHandle", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.out_copy.push_back(state_locked().tz_event.new_handle());
            return 0;
        });
        tcmd(s, 100, "ToCalendarTime", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const int64_t posix = req.arg<int64_t>(0);
            const ipc::Buffer rule = req.in_buf(0);
            if (!rule.addr || rule.size < tz::kRuleSize) return err_tz_conversion();
            tz::CalendarTime cal{};
            tz::CalendarAdditionalInfo info{};
            if (!tz::to_calendar(buf_ptr(rule), posix, &cal, &info)) return err_tz_conversion();
            rsp.push(cal);
            rsp.push(info);
            return 0;
        });
        tcmd(s, 101, "ToCalendarTimeWithMyRule", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const int64_t posix = req.arg<int64_t>(0);
            std::lock_guard<std::mutex> lk(mu());
            tz::CalendarTime cal{};
            tz::CalendarAdditionalInfo info{};
            if (!tz::to_calendar(device_rule(state_locked()), posix, &cal, &info)) return err_tz_conversion();
            NXLOG_T("time", "ToCalendarTimeWithMyRule(%lld) -> %04d-%02d-%02d %02d:%02d:%02d %.8s UTC%+d s", static_cast<long long>(posix),
                    cal.year, cal.month, cal.day, cal.hour, cal.minute, cal.second, info.tz_name, info.utc_offset);
            rsp.push(cal);
            rsp.push(info);
            return 0;
        });
        tcmd(s, 201, "ToPosixTime", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const auto cal = req.arg<tz::CalendarTime>(0);
            const ipc::Buffer rule = req.in_buf(0);
            if (!rule.addr || rule.size < tz::kRuleSize) return err_tz_conversion();
            int64_t times[2] = {0, 0};
            int n = tz::to_posix(buf_ptr(rule), cal, times);
            if (n < 0 || n > 2) n = 0;
            if (n > 0) buf_write(req.out_buf(0), times, static_cast<uint64_t>(n) * 8);
            rsp.push<uint32_t>(static_cast<uint32_t>(n));
            return 0;
        });
        tcmd(s, 202, "ToPosixTimeWithMyRule", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
            const auto cal = req.arg<tz::CalendarTime>(0);
            std::lock_guard<std::mutex> lk(mu());
            int64_t times[2] = {0, 0};
            int n = tz::to_posix(device_rule(state_locked()), cal, times);
            if (n < 0 || n > 2) n = 0;
            NXLOG_T("time", "ToPosixTimeWithMyRule(%04d-%02d-%02d %02d:%02d:%02d) -> %d: %lld %lld (out buffer %llu bytes)", cal.year,
                    cal.month, cal.day, cal.hour, cal.minute, cal.second, n, static_cast<long long>(times[0]),
                    static_cast<long long>(times[1]), static_cast<unsigned long long>(req.out_buf(0).size));
            if (n > 0) buf_write(req.out_buf(0), times, static_cast<uint64_t>(n) * 8);
            rsp.push<uint32_t>(static_cast<uint32_t>(n));
            return 0;
        });
    });
}

std::shared_ptr<ipc::Service> make_static(const char* name, uint32_t perm) {
    return make_iface(name, [perm](ipc::Service& s) {
        tcmd(s, 0, "GetStandardUserSystemClock", [perm](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_clock(Clock::User, perm));
            return 0;
        });
        tcmd(s, 1, "GetStandardNetworkSystemClock", [perm](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_clock(Clock::Network, perm));
            return 0;
        });
        tcmd(s, 2, "GetStandardSteadyClock", [perm](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_steady(perm));
            return 0;
        });
        tcmd(s, 3, "GetTimeZoneService", [perm](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_tz(perm));
            return 0;
        });
        tcmd(s, 4, "GetStandardLocalSystemClock", [perm](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_clock(Clock::Local, perm));
            return 0;
        });
        tcmd(s, 5, "GetEphemeralNetworkSystemClock", [perm](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            rsp.push_object(make_clock(Clock::Ephemeral, perm));
            return 0;
        });
        tcmd(s, 20, "GetSharedMemoryNativeHandle", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            rsp.out_copy.push_back(st.shm->new_handle());
            return 0;
        });
        tcmd(s, 50, "SetStandardSteadyClockInternalOffset", [perm](ipc::Request& req, ipc::Response&) -> uint32_t {
            if (!(perm & kWriteSteady)) return err_perm();
            const int64_t v = req.arg<int64_t>(0);
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            st.internal_ns = v;
            write_shm(st);
            return 0;
        });
        // GetClockSnapshot(u8 type) -> buffer<ClockSnapshot, 0x1a>: from the user and network clock contexts now.
        tcmd(s, 400, "GetClockSnapshot", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const uint8_t type = req.arg<uint8_t>(0);
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            const Now n = now_of(st);
            ClockSnapshot cs{};
            const uint32_t r = make_snapshot(st, sys_ctx(n, st.local_delta), sys_ctx(n, st.network_delta), type, &cs);
            if (r) return r;
            buf_write(req.out_buf(0), &cs, sizeof cs);
            return 0;
        });
        // GetClockSnapshotFromSystemClockContext(u8 type, SystemClockContext user, SystemClockContext network) -> buffer 0x1a.
        // The u8 comes first and the contexts are 8-byte aligned after it (Ryubing reads type, skips 7 bytes, then the contexts).
        tcmd(s, 401, "GetClockSnapshotFromSystemClockContext", [](ipc::Request& req, ipc::Response&) -> uint32_t {
            const uint8_t type = req.arg<uint8_t>(0);
            const SysCtx user = req.arg<SysCtx>(8), network = req.arg<SysCtx>(8 + sizeof(SysCtx));
            std::lock_guard<std::mutex> lk(mu());
            ClockSnapshot cs{};
            const uint32_t r = make_snapshot(state_locked(), user, network, type, &cs);
            if (r) return r;
            buf_write(req.out_buf(0), &cs, sizeof cs);
            return 0;
        });
        tcmd(s, 100, "IsStandardUserSystemClockAutomaticCorrectionEnabled", [](ipc::Request&, ipc::Response& rsp) -> uint32_t {
            std::lock_guard<std::mutex> lk(mu());
            rsp.push<uint8_t>(state_locked().auto_correction ? 1 : 0);
            return 0;
        });
        tcmd(s, 101, "SetStandardUserSystemClockAutomaticCorrectionEnabled", [perm](ipc::Request& req, ipc::Response&) -> uint32_t {
            if (!(perm & kWriteUser)) return err_perm();
            const bool on = req.arg<uint8_t>(0) != 0;
            std::lock_guard<std::mutex> lk(mu());
            State& st = state_locked();
            st.auto_correction = on;
            write_shm(st);
            return 0;
        });
    });
}

}  // namespace

// time:u gets no write rights; time:a the local and user clock and the time zone; time:s the network clock; time:r the steady clock.
void register_time() {
    struct Flavour {
        const char* name;
        uint32_t perm;
    };
    static constexpr Flavour kFlavours[] = {
        {"time:u", 0},
        {"time:a", kWriteLocal | kWriteUser | kWriteTimeZone},
        {"time:s", kWriteNetwork},
        {"time:r", kWriteSteady},
    };
    for (const Flavour& f : kFlavours) {
        const char* name = f.name;
        const uint32_t perm = f.perm;
        add(name, [name, perm] { return make_static(name, perm); });
    }
}

// ---- the game clock, moved while the game runs -------------------------------------------------------------------------------
// The same offset as [time] clock_offset, but set at run time (the Time tab of the in-game Options): the game sees it as the player setting
// the console's date and time by hand, so it is told the clocks changed.
namespace timetravel {
int64_t offset() {
    std::lock_guard<std::mutex> lk(mu());
    return state_locked().local_delta;
}
int64_t real_now() { return host_unix_now(); }

// The game's view of a unix time under the device zone rule. v = year, month, day, hour, minute, second.
bool local_calendar(int64_t posix, int v[6]) {
    std::lock_guard<std::mutex> lk(mu());
    tz::CalendarTime cal{};
    tz::CalendarAdditionalInfo info{};
    if (!tz::to_calendar(device_rule(state_locked()), posix, &cal, &info)) return false;
    v[0] = cal.year; v[1] = cal.month; v[2] = cal.day; v[3] = cal.hour; v[4] = cal.minute; v[5] = cal.second;
    return true;
}

// The unix time of a date and time in the device zone (the inverse of local_calendar); false for one that does not exist.
bool posix_of_local(int y, int mo, int d, int h, int mi, int64_t* out) {
    std::lock_guard<std::mutex> lk(mu());
    tz::CalendarTime cal{};
    cal.year = static_cast<int16_t>(y); cal.month = static_cast<int8_t>(mo); cal.day = static_cast<int8_t>(d);
    cal.hour = static_cast<int8_t>(h); cal.minute = static_cast<int8_t>(mi); cal.second = 0;
    int64_t times[2] = {0, 0};
    const int n = tz::to_posix(device_rule(state_locked()), cal, times);
    if (n < 1 || n > 2) return false;
    *out = times[0];
    return true;
}

// Moves the game clock to `seconds` from the host clock (0 = real time), limited to +-10 years like [time] clock_offset.
void set_offset(int64_t seconds) {
    constexpr int64_t kMax = 10ll * 366 * 24 * 3600;
    seconds = seconds < -kMax ? -kMax : seconds > kMax ? kMax : seconds;
    std::lock_guard<std::mutex> lk(mu());
    State& s = state_locked();
    if (seconds == s.local_delta) return;
    s.local_delta = seconds;
    if (s.scope_all) s.network_delta = s.ephemeral_delta = seconds;
    write_shm(s);
    s.clock_event.signal();
    NXLOG_I("time", "game clock: now %+lld s from the host clock (host %lld)", static_cast<long long>(seconds), static_cast<long long>(host_unix_now()));
}
}  // namespace timetravel

void time_reset() {
    std::lock_guard<std::mutex> lk(mu());
    State& s = raw_state();
    if (s.created) init_locked(s);
}

}  // namespace nxrt::services
