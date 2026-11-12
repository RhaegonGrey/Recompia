// SPDX-License-Identifier: GPL-3.0-or-later
// audout:u and audin:u (S3c): the output and capture devices an application opens for PCM streams.
// Adapted from ScarletNX src/nx/hle/services/audio.c (GPL-3.0-or-later; that file's audout part is itself ported from Eden,
// GPL-2.0-or-later, as its header says): the command table, the buffer protocol (appended tags, released tags, the buffer event,
// the played-sample count), the refusal of a second Start, and the release of queued buffers on Stop and Flush.
// Adapted from ryubing src/Ryujinx.Horizon/Sdk/Audio/Detail/AudioOut.cs and AudioOutManager.cs (MIT): the channel-count rule of
// OpenAudioOut and the Flush result.
// Layouts and result codes: https://switchbrew.org/wiki/Audio_services and https://switchbrew.org/wiki/Error_codes (module 153).
// Playback runs on one thread per started stream (Pro Audio priority, 1 ms timer, 1 ms poll), as the DSP does on hardware: every
// appended buffer goes to nxrt::audio::sink() as soon as it is appended, and its tag is released when the sink's own device clock has
// played it, less [audio] out_latency_ms (default 30): the guest refills that much early, so the sink always holds the guest's queue
// plus that margin and never starves between two guest buffers (XAudio2 pulls 10 ms at a time). A sink without a device clock (null),
// a sink that was reopened under the stream, or a device that has not moved for 200 ms falls back to the host clock on an absolute
// timeline (buffer k ends at the first buffer's start plus the frames before it). Capture (audin) is silent: each appended buffer is
// zeroed when it starts and released at real time.
// The previous design released each buffer frames / rate after the moment its pacing thread woke up for it and only then wrote the
// next one to the sink: every late wake-up (15.6 ms without a 1 ms timer) stretched the stream, and the sink held one buffer at a
// time, so the device ran dry between buffers.
#include "services_internal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "audren/Integration/ThreadBoost.h"
#include "nxrt/audio_sink.h"
#include "nxrt/config.h"
#include "nxrt/gauge.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"

namespace nxrt::services {
namespace {

using Clock = std::chrono::steady_clock;

constexpr char kOutName[] = "DeviceOut";
constexpr char kInName[] = "BuiltInHeadset";
constexpr uint32_t kNameSize = 0x100;        // an AudioDeviceName (also AudioOutInfo / AudioInInfo)
constexpr uint32_t kBufferSize = 0x28;       // AudioOutBuffer / AudioInBuffer: next, buffer, capacity, size, offset (Switchbrew)
constexpr uint32_t kDefaultRate = 48000;
constexpr uint32_t kMinRate = 8000, kMaxRate = 192000;
constexpr uint32_t kMaxBuffers = 64;        // queued plus released-but-not-taken buffers per stream (ScarletNX AUDOUT_BUFFERS)
constexpr uint32_t kFormatPcmInt16 = 2;     // nn::audio::SampleFormat PcmInt16
constexpr uint32_t kStateStarted = 0, kStateStopped = 1;
constexpr uint32_t kResNotFound = res(153, 1);
constexpr uint32_t kResOperationFailed = res(153, 2);
constexpr uint32_t kResBufferCountFull = res(153, 8);

// nn::audio::AudioOutBuffer (AudioInBuffer has the same layout). Written out from Switchbrew's table.
struct Desc {
    uint64_t next = 0;      // 0x00 unused
    uint64_t buffer = 0;    // 0x08 guest pointer to the samples
    uint64_t capacity = 0;  // 0x10
    uint64_t size = 0;      // 0x18 bytes of data
    uint64_t offset = 0;    // 0x20 offset of the data in the samples buffer
};

// ---- guest memory --------------------------------------------------------------------------------------------------------
// Guest pointers are host pointers: each range is checked with nxrt::mem::is_mapped before it is read or written.
bool guest_readable(uint64_t addr, uint64_t n) { return addr != 0 && (n == 0 || nxrt::mem::is_mapped(addr, n)); }

bool read_desc(const ipc::Buffer& b, Desc& d) {
    d = Desc{};
    const uint64_t n = b.size < kBufferSize ? b.size : kBufferSize;   // a shorter descriptor leaves the missing fields zero
    if (!guest_readable(b.addr, n)) return false;
    buf_read(b, &d, n);
    return true;
}

std::string read_name(const ipc::Buffer& b) {
    const uint64_t n = b.size < kNameSize ? b.size : kNameSize;
    if (n == 0 || !guest_readable(b.addr, n)) return {};
    std::string s(static_cast<size_t>(n), '\0');
    buf_read(b, s.data(), n);
    return std::string(s.c_str());
}

// Writes a device name zero padded, as many bytes as the buffer holds (at most 0x100).
bool write_name(const ipc::Buffer& b, const char* name) {
    std::array<uint8_t, kNameSize> out{};
    std::memcpy(out.data(), name, std::min<size_t>(std::strlen(name), kNameSize - 1));
    const uint64_t n = b.size < kNameSize ? b.size : kNameSize;
    if (n == 0 || !guest_readable(b.addr, n)) return false;
    buf_write(b, out.data(), n);
    return true;
}

// ---- formats the devices accept -------------------------------------------------------------------------------------------
// The rate an out or in plays at: the default 48 kHz when none (or an unsupported one) is asked for.
uint32_t pick_rate(uint32_t asked) { return asked < kMinRate || asked > kMaxRate ? kDefaultRate : asked; }
// The sink plays 2 or 6 channels (audio_sink.h); any other count falls back to stereo, as Ryujinx does.
uint32_t pick_out_channels(uint32_t asked) { return asked == 6 ? 6 : 2; }
uint32_t pick_in_channels(uint32_t asked) { return asked == 1 || asked == 4 || asked == 6 ? asked : 2; }

// ---- host sink ------------------------------------------------------------------------------------------------------------
// One sink for every stream: it is opened on the first start with the work's rate and channel count, and opened again when
// a stream asks for another format or the active sink was replaced (nxrt::audio::set_sink).
struct HostSink {
    std::mutex mu;                                   // guards the fields below and every call into the sink
    std::shared_ptr<nxrt::audio::AudioSink> sink;    // the sink opened last
    uint32_t rate = 0, channels = 0;
    bool open_ok = false;
    uint64_t written = 0;   // frames the sink accepted since it was opened: the sink's write position
    uint64_t gen = 0;       // bumped on every (re)open, so a stream knows its positions belong to another opening
};
HostSink& host() {
    static HostSink h;
    return h;
}

// Caller holds host().mu. True when the sink is open for this format.
bool open_host_locked(uint32_t rate, uint32_t channels) {
    HostSink& h = host();
    auto cur = nxrt::audio::sink();
    if (cur == h.sink && rate == h.rate && channels == h.channels) return h.open_ok;
    h.sink = cur;
    h.rate = rate;
    h.channels = channels;
    h.open_ok = cur->open(rate, channels);
    h.written = 0;
    h.gen++;
    if (h.open_ok) NXLOG_I("audout", "host sink %s open at %u Hz, %u channels", cur->name(), rate, channels);
    else NXLOG_W("audout", "host sink %s would not open at %u Hz, %u channels: playing without sound", cur->name(), rate, channels);
    return h.open_ok;
}

// Where a buffer ended up in the sink: its end in the sink's write position, and the opening that position belongs to.
struct HostMark {
    bool device = false;   // the sink has a device clock, so the mark can be compared with host_played()
    uint64_t gen = 0;
    uint64_t end = 0;
};

HostMark play_host(uint32_t rate, uint32_t channels, const int16_t* pcm, uint32_t frames) {
    std::lock_guard<std::mutex> lk(host().mu);
    HostMark m;
    if (!open_host_locked(rate, channels)) return m;
    HostSink& h = host();
    h.written += h.sink->write(pcm, frames);
    m.device = h.sink->has_device_clock();
    m.gen = h.gen;
    m.end = h.written;
    return m;
}

// The sink's play position (frames it has played since it was opened) while it is still the opening `gen` and has a device clock.
bool host_played(uint64_t gen, uint64_t* pos) {
    std::lock_guard<std::mutex> lk(host().mu);
    HostSink& h = host();
    if (!h.open_ok || !h.sink || h.gen != gen || h.sink != nxrt::audio::sink() || !h.sink->has_device_clock()) return false;
    const uint64_t q = h.sink->queued_frames();
    *pos = h.written > q ? h.written - q : 0;
    return true;
}

// [audio] out_latency_ms (0..200, default 30): how far ahead of the device's play position audout releases buffers to the guest.
uint32_t out_latency_ms() {
    static const uint32_t ms = [] {
        const int64_t v = nxrt::config_get_int("audio", "out_latency_ms", 30);
        return static_cast<uint32_t>(v < 0 ? 0 : (v > 200 ? 200 : v));
    }();
    return ms;
}

// Gauges (include/nxrt/gauge.h), summed over every open stream: buffers appended and not yet played, and played buffers whose tag the
// guest has not taken back yet. Each stream changes them where it moves a buffer between the two lists, and gives its share back when destroyed.
GaugeCounter g_buffers_queued("audout.buffers_queued");
GaugeCounter g_buffers_released("audout.buffers_released");

// ---- streams --------------------------------------------------------------------------------------------------------------
enum class Dir { Out, In };

// One buffer appended by the guest. `samples` is 0 when the samples cannot be read (played as silence).
struct Entry {
    uint64_t tag = 0;
    uint64_t samples = 0;
    uint64_t bytes = 0;
    uint32_t frames = 0;
    bool submitted = false;   // written to the sink (out) or zeroed (in)
    HostMark mark;            // where it ended in the sink (out)
    uint64_t stream_end = 0;  // the stream's frame count at the end of this buffer (the host-clock timeline)
};

// An open audio out or in: its queue, its released tags, its pacing thread and its buffer event. Shared by the IAudio* object
// (the service) and the registry; the thread only uses it while the object is alive, because every path that drops the object
// stops the thread first (on_close, the destructor).
class Stream {
public:
    Stream(Dir dir, uint32_t rate, uint32_t channels) : dir_(dir), rate_(rate), channels_(channels) {}
    ~Stream() {
        shutdown();
        g_buffers_queued.sub(static_cast<int64_t>(queue_.size()));
        g_buffers_released.sub(static_cast<int64_t>(released_.size()));
    }
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    Dir dir() const { return dir_; }

    // Guest commands. Each takes the work's locks itself.
    uint32_t start();
    uint32_t stop();
    bool flush();
    uint32_t append(uint64_t tag, const Desc& d);
    uint32_t take_released(uint64_t* out, uint32_t max);
    bool contains(uint64_t tag);
    uint32_t queued();
    uint64_t played();
    uint32_t state();
    float volume();
    void set_volume(float v);
    uint32_t new_event_handle() { return ev_.new_handle(); }

    // Stops the pacing thread and releases what is queued. Used by on_close, reset and the destructor. Idempotent.
    void shutdown();

private:
    void pace();
    HostMark play(const Entry& e);
    void halt();                       // stops and joins the pacing thread; caller holds life_
    void release_all_locked();         // every queued buffer is released now (unplayed)
    void sync_event_locked();          // the buffer event is signalled while a released tag is waiting

    const Dir dir_;
    const uint32_t rate_;
    const uint32_t channels_;

    std::mutex life_;                  // serialises start / stop / shutdown (the thread never takes it)
    std::mutex mu_;                    // guards everything below
    std::condition_variable cv_;
    std::deque<Entry> queue_;          // appended and not yet released; the front is playing when started
    std::deque<uint64_t> released_;    // released tags the guest has not taken yet
    bool started_ = false;
    uint64_t epoch_ = 0;               // bumped when the queue is emptied, so the pacing thread re-reads its head
    uint64_t played_ = 0;             // frames released as played
    uint64_t submitted_frames_ = 0;    // frames of every buffer submitted since the stream was made (the host-clock timeline)
    bool logged_first_ = false;
    float volume_ = 1.0f;
    std::thread th_;
    ServiceEvent ev_;                  // readable while released_ is not empty
};

struct Registry {
    std::mutex mu;
    std::vector<std::weak_ptr<Stream>> streams;
};
Registry& registry() {
    static Registry r;
    return r;
}

std::shared_ptr<Stream> new_stream(Dir dir, uint32_t rate, uint32_t channels) {
    auto s = std::make_shared<Stream>(dir, rate, channels);
    Registry& r = registry();
    std::lock_guard<std::mutex> lk(r.mu);
    std::erase_if(r.streams, [](const std::weak_ptr<Stream>& w) { return w.expired(); });
    r.streams.push_back(s);
    return s;
}

uint32_t Stream::start() {
    std::lock_guard<std::mutex> life(life_);
    std::lock_guard<std::mutex> lk(mu_);
    if (started_) return kResOperationFailed;
    if (dir_ == Dir::Out) {
        std::lock_guard<std::mutex> hl(host().mu);
        open_host_locked(rate_, channels_);
    }
    started_ = true;
    th_ = std::thread([this] { pace(); });
    return 0;
}

uint32_t Stream::stop() {
    std::lock_guard<std::mutex> life(life_);
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!started_) return 0;   // a stopped stream keeps what was appended until it starts
    }
    halt();
    std::lock_guard<std::mutex> lk(mu_);
    release_all_locked();
    return 0;
}

void Stream::shutdown() {
    std::lock_guard<std::mutex> life(life_);
    halt();
    std::lock_guard<std::mutex> lk(mu_);
    release_all_locked();
}

void Stream::halt() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        started_ = false;
        ++epoch_;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

bool Stream::flush() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) return false;
    release_all_locked();
    return true;
}

uint32_t Stream::append(uint64_t tag, const Desc& d) {
    Entry e;
    e.tag = tag;
    e.bytes = d.size;
    e.frames = static_cast<uint32_t>(std::min<uint64_t>(d.size / (channels_ * 2u), 0xFFFFFFFFu));
    const bool in_range = d.offset <= d.capacity && d.size <= d.capacity - d.offset;
    if (in_range && d.size && guest_readable(d.buffer + d.offset, d.size)) {
        e.samples = d.buffer + d.offset;
    } else if (d.size) {
        NXLOG_W("audout", "sample buffer %#llx (%llu bytes at %llu of %llu) is not readable: played as silence",
                static_cast<unsigned long long>(d.buffer), static_cast<unsigned long long>(d.size),
                static_cast<unsigned long long>(d.offset), static_cast<unsigned long long>(d.capacity));
    }
    std::lock_guard<std::mutex> lk(mu_);
    if (queue_.size() + released_.size() >= kMaxBuffers) return kResBufferCountFull;
    queue_.push_back(e);
    g_buffers_queued.add(1);
    cv_.notify_all();
    return 0;
}

uint32_t Stream::take_released(uint64_t* out, uint32_t max) {
    std::lock_guard<std::mutex> lk(mu_);
    uint32_t n = 0;
    while (n < max && !released_.empty()) {
        out[n++] = released_.front();
        released_.pop_front();
        g_buffers_released.sub(1);
    }
    sync_event_locked();
    return n;
}

bool Stream::contains(uint64_t tag) {
    std::lock_guard<std::mutex> lk(mu_);
    for (const Entry& e : queue_)
        if (e.tag == tag) return true;
    for (uint64_t t : released_)
        if (t == tag) return true;
    return false;
}

uint32_t Stream::queued() {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<uint32_t>(queue_.size());
}

uint64_t Stream::played() {
    std::lock_guard<std::mutex> lk(mu_);
    return played_;
}

uint32_t Stream::state() {
    std::lock_guard<std::mutex> lk(mu_);
    return started_ ? kStateStarted : kStateStopped;
}

float Stream::volume() {
    std::lock_guard<std::mutex> lk(mu_);
    return volume_;
}

void Stream::set_volume(float v) {
    std::lock_guard<std::mutex> lk(mu_);
    volume_ = v;
}

void Stream::release_all_locked() {
    for (const Entry& e : queue_) released_.push_back(e.tag);
    g_buffers_queued.sub(static_cast<int64_t>(queue_.size()));
    g_buffers_released.add(static_cast<int64_t>(queue_.size()));
    queue_.clear();
    ++epoch_;
    cv_.notify_all();
    sync_event_locked();
}

void Stream::sync_event_locked() {
    if (released_.empty()) ev_.clear();
    else ev_.signal();
}

// The samples of one buffer: written to the sink (out) or zeroed (in). Runs without mu_ held.
HostMark Stream::play(const Entry& e) {
    if (e.frames == 0) return {};
    const uint64_t n = static_cast<uint64_t>(e.frames) * channels_ * 2u;
    const bool readable = e.samples != 0 && nxrt::mem::is_mapped(e.samples, n);
    if (dir_ == Dir::In) {
        if (readable) std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(e.samples)), 0, static_cast<size_t>(n));
        return {};
    }
    std::vector<int16_t> pcm(static_cast<size_t>(e.frames) * channels_, 0);   // silence when the samples cannot be read
    if (readable) std::memcpy(pcm.data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(e.samples)), static_cast<size_t>(n));
    return play_host(rate_, channels_, pcm.data(), e.frames);
}

// The stream thread. Each pass submits every buffer appended since the last pass, then releases the buffers at the front that have
// been played. Out streams on a sink with a device clock follow that clock (less the out_latency_ms margin); everything else follows
// the host clock on an absolute timeline that starts with the first buffer after the queue ran empty, so a late wake-up never
// stretches the stream. Halt, flush and stop bump epoch_, which sends the loop back to re-read the queue.
void Stream::pace() {
    nxrt::log_set_thread_name(dir_ == Dir::Out ? "audio-out" : "audio-in");
    nxrt::audren::BoostAudioThread();
    nxrt::audren::TimerResolutionScope::Acquire();
    const uint64_t margin = dir_ == Dir::Out ? static_cast<uint64_t>(rate_) * out_latency_ms() / 1000u : 0;
    constexpr auto kStall = std::chrono::milliseconds(200);   // a device position that has not moved this long is not trusted

    std::unique_lock<std::mutex> lk(mu_);
    double clock_pos = 0.0;          // host-clock play position on the stream timeline (frames)
    Clock::time_point clock_at{};    // when clock_pos was last advanced
    uint64_t dev_pos = 0;            // last device position seen, its opening, and when it last moved
    uint64_t dev_gen = 0;
    Clock::time_point dev_moved{};
    bool idle = true;                // nothing submitted and unreleased: the clocks restart with the next buffer

    while (started_) {
        if (queue_.empty()) {
            idle = true;
            cv_.wait(lk, [this] { return !started_ || !queue_.empty(); });
            continue;
        }
        const uint64_t epoch = epoch_;

        // Submit what was appended, in order. References into the deque stay valid while epoch_ is unchanged (only this thread pops,
        // every other removal bumps epoch_, and push_back keeps references).
        bool restart = false;
        for (size_t i = 0; i < queue_.size(); i++) {
            if (queue_[i].submitted) continue;
            if (idle) {
                clock_pos = static_cast<double>(submitted_frames_);
                clock_at = Clock::now();
                dev_moved = clock_at;
                idle = false;
            }
            const Entry copy = queue_[i];
            lk.unlock();
            const HostMark m = play(copy);
            lk.lock();
            if (!started_ || epoch_ != epoch) {
                restart = true;
                break;
            }
            Entry& e = queue_[i];
            e.submitted = true;
            e.mark = m;
            submitted_frames_ += e.frames;
            e.stream_end = submitted_frames_;
            if (!logged_first_ && dir_ == Dir::Out && e.frames) {
                logged_first_ = true;
                NXLOG_I("audout", "first buffer: %u frames (%.1f ms); release margin %u ms", e.frames, e.frames * 1000.0 / rate_,
                        out_latency_ms());
            }
        }
        if (restart) continue;

        // The host clock advances while something is submitted, never past what was submitted.
        const Clock::time_point now = Clock::now();
        clock_pos += std::chrono::duration<double>(now - clock_at).count() * rate_;
        clock_at = now;
        if (clock_pos > static_cast<double>(submitted_frames_)) clock_pos = static_cast<double>(submitted_frames_);

        // Release the played buffers at the front.
        while (!queue_.empty() && queue_.front().submitted) {
            const Entry& head = queue_.front();
            bool done = false;
            if (head.mark.device) {
                const uint64_t gen = head.mark.gen, end = head.mark.end;
                uint64_t pos = 0;
                lk.unlock();
                const bool ok = host_played(gen, &pos);
                lk.lock();
                if (!started_ || epoch_ != epoch) break;
                if (ok) {
                    if (gen != dev_gen || pos != dev_pos) {
                        dev_gen = gen;
                        dev_pos = pos;
                        dev_moved = now;
                    }
                    if (pos + margin >= end) {
                        done = true;
                    } else if (now - dev_moved < kStall) {
                        break;   // the device is playing toward it
                    }
                }
                // Not ok (the sink was replaced or reopened) or a stalled device: the host clock decides.
            }
            const Entry& h2 = queue_.front();
            if (!done && clock_pos + 0.5 < static_cast<double>(h2.stream_end)) break;
            played_ += h2.frames;
            released_.push_back(h2.tag);
            queue_.pop_front();
            g_buffers_queued.sub(1);
            g_buffers_released.add(1);
            sync_event_locked();
        }
        if (!started_ || epoch_ != epoch) continue;

        // Poll at 1 ms (the timer runs at 1 ms while a stream plays); an append or a stop wakes the thread at once.
        cv_.wait_for(lk, std::chrono::milliseconds(1), [this, epoch] {
            if (!started_ || epoch_ != epoch) return true;
            for (const Entry& e : queue_)
                if (!e.submitted) return true;
            return false;
        });
    }
    lk.unlock();
    nxrt::audren::TimerResolutionScope::Release();
}

// ---- services -------------------------------------------------------------------------------------------------------------
// The IAudioOut / IAudioIn object. Its stream is shut down (pacing thread joined) when the session that holds it goes away.
class StreamService : public ipc::Service {
public:
    StreamService(const char* name, std::shared_ptr<Stream> s) : ipc::Service(name), st_(std::move(s)) {}
    void on_close() override { st_->shutdown(); }

private:
    std::shared_ptr<Stream> st_;
};

// AppendAudioOutBuffer / AppendAudioInBuffer (and the Auto forms): tag, then the AudioOutBuffer descriptor in buffer A.
uint32_t cmd_append(Stream& s, ipc::Request& req) {
    Desc d;
    if (!read_desc(req.in_buf(0), d)) return kResOperationFailed;
    return s.append(req.arg<uint64_t>(0), d);
}

// GetReleasedAudioOutBuffers / GetReleasedAudioInBuffers (and the Auto forms): fills the tag array in buffer B and returns the count.
// The array is zeroed past the tags, as the SDK reads every slot.
uint32_t cmd_released(Stream& s, ipc::Request& req, ipc::Response& rsp) {
    const ipc::Buffer b = req.out_buf(0);
    const uint64_t bytes = b.size < kMaxBuffers * 8u ? b.size / 8 * 8 : kMaxBuffers * 8u;
    if (bytes == 0 || !guest_readable(b.addr, bytes)) {
        rsp.push<uint32_t>(0);
        return 0;
    }
    std::array<uint64_t, kMaxBuffers> tags{};
    const uint32_t n = s.take_released(tags.data(), static_cast<uint32_t>(bytes / 8));
    buf_write(b, tags.data(), bytes);
    rsp.push<uint32_t>(n);
    return 0;
}

std::shared_ptr<ipc::Service> make_out_stream(std::shared_ptr<Stream> st) {
    Stream* s = st.get();
    auto svc = std::make_shared<StreamService>("IAudioOut", std::move(st));
    svc->add_cmd(0, "GetAudioOutState", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(s->state());
        return 0;
    });
    svc->add_cmd(1, "Start", [s](ipc::Request&, ipc::Response&) -> uint32_t {
        const uint32_t rc = s->start();
        if (rc == 0) NXLOG_I("audout", "Start");
        return rc;
    });
    svc->add_cmd(2, "Stop", [s](ipc::Request&, ipc::Response&) -> uint32_t { return s->stop(); });
    svc->add_cmd(3, "AppendAudioOutBuffer", [s](ipc::Request& req, ipc::Response&) -> uint32_t { return cmd_append(*s, req); });
    svc->add_cmd(4, "RegisterBufferEvent", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.out_copy.push_back(s->new_event_handle());
        return 0;
    });
    svc->add_cmd(5, "GetReleasedAudioOutBuffers", [s](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        return cmd_released(*s, req, rsp);
    });
    svc->add_cmd(6, "ContainsAudioOutBuffer", [s](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(s->contains(req.arg<uint64_t>(0)) ? 1 : 0);
        return 0;
    });
    svc->add_cmd(7, "AppendAudioOutBufferAuto", [s](ipc::Request& req, ipc::Response&) -> uint32_t { return cmd_append(*s, req); });
    svc->add_cmd(8, "GetReleasedAudioOutBuffersAuto", [s](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        return cmd_released(*s, req, rsp);
    });
    svc->add_cmd(9, "GetAudioOutBufferCount", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(s->queued());
        return 0;
    });
    svc->add_cmd(10, "GetAudioOutPlayedSampleCount", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint64_t>(s->played());
        return 0;
    });
    svc->add_cmd(11, "FlushAudioOutBuffers", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(s->flush() ? 1 : 0);
        return 0;
    });
    svc->add_cmd(12, "SetAudioOutVolume", [s](ipc::Request& req, ipc::Response&) -> uint32_t {
        s->set_volume(req.arg<float>(0));
        return 0;
    });
    svc->add_cmd(13, "GetAudioOutVolume", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<float>(s->volume());
        return 0;
    });
    return svc;
}

// IAudioIn: the buffer protocol of IAudioOut (no UAC buffers and no device gain: not implemented).
std::shared_ptr<ipc::Service> make_in_stream(std::shared_ptr<Stream> st) {
    Stream* s = st.get();
    auto svc = std::make_shared<StreamService>("IAudioIn", std::move(st));
    svc->add_cmd(0, "GetAudioInState", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(s->state());
        return 0;
    });
    svc->add_cmd(1, "Start", [s](ipc::Request&, ipc::Response&) -> uint32_t {
        const uint32_t rc = s->start();
        if (rc == 0) NXLOG_I("audin", "Start");
        return rc;
    });
    svc->add_cmd(2, "Stop", [s](ipc::Request&, ipc::Response&) -> uint32_t { return s->stop(); });
    svc->add_cmd(3, "AppendAudioInBuffer", [s](ipc::Request& req, ipc::Response&) -> uint32_t { return cmd_append(*s, req); });
    svc->add_cmd(4, "RegisterBufferEvent", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.out_copy.push_back(s->new_event_handle());
        return 0;
    });
    svc->add_cmd(5, "GetReleasedAudioInBuffers", [s](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        return cmd_released(*s, req, rsp);
    });
    svc->add_cmd(6, "ContainsAudioInBuffer", [s](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(s->contains(req.arg<uint64_t>(0)) ? 1 : 0);
        return 0;
    });
    svc->add_cmd(8, "AppendAudioInBufferAuto", [s](ipc::Request& req, ipc::Response&) -> uint32_t { return cmd_append(*s, req); });
    svc->add_cmd(9, "GetReleasedAudioInBuffersAuto", [s](ipc::Request& req, ipc::Response& rsp) -> uint32_t {
        return cmd_released(*s, req, rsp);
    });
    svc->add_cmd(11, "GetAudioInBufferCount", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint32_t>(s->queued());
        return 0;
    });
    svc->add_cmd(14, "FlushAudioInBuffers", [s](ipc::Request&, ipc::Response& rsp) -> uint32_t {
        rsp.push<uint8_t>(s->flush() ? 1 : 0);
        return 0;
    });
    return svc;
}

// ListAudioOuts / ListAudioIns (and the Auto forms): one name when the output buffer holds one, the count returned.
uint32_t list_names(ipc::Request& req, ipc::Response& rsp, const char* name) {
    uint32_t count = 0;
    if (write_name(req.out_buf(0), name)) count = 1;
    rsp.push<uint32_t>(count);
    return 0;
}

// OpenAudioOut: AudioOutParameter {u32 SampleRate, u16 ChannelCount, u16 reserved} at 0, the applet resource id at 8 (not used).
// Returns AudioOutParameterInternal {rate, channels, sample format, state} and the IAudioOut object.
uint32_t open_out(ipc::Request& req, ipc::Response& rsp) {
    const std::string want = read_name(req.in_buf(0));
    if (!want.empty() && want != kOutName) return kResNotFound;
    const uint32_t rate = pick_rate(req.arg<uint32_t>(0));
    const uint32_t ch = pick_out_channels(req.arg<uint16_t>(4));
    NXLOG_I("audout", "OpenAudioOut: %s, %u Hz, %u channels", kOutName, rate, ch);
    write_name(req.out_buf(0), kOutName);
    rsp.push<uint32_t>(rate);
    rsp.push<uint32_t>(ch);
    rsp.push<uint32_t>(kFormatPcmInt16);
    rsp.push<uint32_t>(kStateStopped);
    rsp.push_object(make_out_stream(new_stream(Dir::Out, rate, ch)));
    return 0;
}

// OpenAudioIn: AudioInParameter, same layout as AudioOutParameter. Returns AudioInParameterInternal and the IAudioIn object.
uint32_t open_in(ipc::Request& req, ipc::Response& rsp) {
    const std::string want = read_name(req.in_buf(0));
    if (!want.empty() && want != kInName) return kResNotFound;
    const uint32_t rate = pick_rate(req.arg<uint32_t>(0));
    const uint32_t ch = pick_in_channels(req.arg<uint16_t>(4));
    NXLOG_I("audin", "OpenAudioIn: %s, %u Hz, %u channels", kInName, rate, ch);
    write_name(req.out_buf(0), kInName);
    rsp.push<uint32_t>(rate);
    rsp.push<uint32_t>(ch);
    rsp.push<uint32_t>(kFormatPcmInt16);
    rsp.push<uint32_t>(kStateStopped);
    rsp.push_object(make_in_stream(new_stream(Dir::In, rate, ch)));
    return 0;
}

std::shared_ptr<ipc::Service> make_audout() {
    return make_iface("audout:u", [](ipc::Service& s) {
        s.add_cmd(0, "ListAudioOuts", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return list_names(req, rsp, kOutName); });
        s.add_cmd(1, "OpenAudioOut", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return open_out(req, rsp); });
        s.add_cmd(2, "ListAudioOutsAuto", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return list_names(req, rsp, kOutName); });
        s.add_cmd(3, "OpenAudioOutAuto", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return open_out(req, rsp); });
    });
}

std::shared_ptr<ipc::Service> make_audin() {
    return make_iface("audin:u", [](ipc::Service& s) {
        s.add_cmd(0, "ListAudioIns", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return list_names(req, rsp, kInName); });
        s.add_cmd(1, "OpenAudioIn", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return open_in(req, rsp); });
        s.add_cmd(2, "ListAudioInsAuto", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return list_names(req, rsp, kInName); });
        s.add_cmd(3, "OpenAudioInAuto", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return open_in(req, rsp); });
        s.add_cmd(4, "ListAudioInsAutoFiltered", [](ipc::Request& req, ipc::Response& rsp) -> uint32_t { return list_names(req, rsp, kInName); });
    });
}

}  // namespace

std::shared_ptr<ipc::Service> audio_out_manager() { return make_audout(); }
std::shared_ptr<ipc::Service> audio_in_manager() { return make_audin(); }

void register_audio_out() {
    add("audout:u", [] { return make_audout(); });
    add("audin:u", [] { return make_audin(); });
}

// Stops every pacing thread and forgets the streams. Sessions still holding an object keep a working but unregistered stream.
void audio_out_reset() {
    std::vector<std::shared_ptr<Stream>> live;
    {
        Registry& r = registry();
        std::lock_guard<std::mutex> lk(r.mu);
        for (const auto& w : r.streams)
            if (auto s = w.lock()) live.push_back(std::move(s));
        r.streams.clear();
    }
    for (auto& s : live) s->shutdown();
    {
        std::lock_guard<std::mutex> lk(host().mu);
        host().sink.reset();
        host().rate = 0;
        host().channels = 0;
        host().open_ok = false;
    }
}

}  // namespace nxrt::services
