// SPDX-License-Identifier: GPL-3.0-or-later
// XAudio2 host sink (S3c): an AudioSink that plays the guest's interleaved int16 PCM on the output device chosen by
// [audio] device (the default device unless it names another one).
// The engine, a mastering voice and one source voice are created in open(); write() copies the caller's frames into a small pool
// of buffers and submits them; the voice callback (OnBufferEnd, on XAudio2's audio thread) returns the buffer to the pool. When the
// device already holds kMaxQueued buffers, or the pool is empty, write() drops the rest and reports how many frames it accepted, so
// the caller never blocks. The voice callback also counts underruns (OnVoiceProcessingPassStart with nothing queued). The source
// voice carries the process master volume ([audio] volume), re-applied whenever it changes.
// Device loss: XAudio2 reports a lost device (unplugged headphones, Bluetooth gone, the HDMI monitor off) through
// IXAudio2EngineCallback::OnCriticalError, or SubmitSourceBuffer fails with XAUDIO2_E_DEVICE_INVALIDATED. The callback only flags it;
// the next write() (the only thread that touches the voices while playing) destroys the dead voices and engine and switches to a
// StandInClock (audio_standin.h), a silent queue that drains at the sample rate, so the renderer keeps its pacing and the game never
// notices. A worker thread then retries the whole open (1, 2, 4, then every 5 s, on the then-current default device) and the next
// write() adopts the new device. With no loss, write() pays one atomic load.
#include <windows.h>
#include <objbase.h>
#include <mmreg.h>
#include <xaudio2.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/audio_standin.h"
#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/text.h"

#pragma comment(lib, "xaudio2.lib")

namespace nxrt::audio {
namespace {

// audout submits each guest buffer as its own voice buffer (often 5-10 ms each) and keeps the guest's queue plus [audio] out_latency_ms in
// the voice, so the pool holds far more buffers than the queue normally needs.
constexpr uint32_t kSlots = 64;          // buffers in the pool
constexpr uint32_t kSlotFrames = 1024;   // frames per buffer (about 21 ms at 48 kHz)
constexpr UINT32 kMaxQueued = 60;        // beyond this many queued buffers write() drops the new frames (below XAUDIO2_MAX_QUEUED_BUFFERS)

// KSDATAFORMAT_SUBTYPE_PCM (ksmedia.h), spelled out so the link needs no ksguid.lib.
constexpr GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
// SPEAKER_5POINT1: FRONT_LEFT | FRONT_RIGHT | FRONT_CENTER | LOW_FREQUENCY | BACK_LEFT | BACK_RIGHT.
constexpr DWORD kChannelMask51 = 0x3F;

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

using text::lower;

void log_devices(const std::vector<OutputDevice>& devices) {
    if (devices.empty()) NXLOG_I("audio", "xaudio2: no active output endpoints");
    for (const OutputDevice& d : devices) {
        NXLOG_I("audio", "xaudio2: output device%s: %s [%s]", d.is_default ? " (default)" : "", d.name.c_str(),
                to_utf8(d.id).c_str());
    }
}

// The endpoint [audio] device asks for. "default" (or empty) is the default endpoint (empty id). Otherwise an exact endpoint id
// wins, then the first endpoint whose friendly name contains the text (ASCII case-insensitive). No match: a warning and the default.
std::wstring pick_device(const std::vector<OutputDevice>& devices, const std::string& want) {
    if (want.empty() || lower(want) == "default") return {};
    for (const OutputDevice& d : devices) {
        if (to_utf8(d.id) == want) return d.id;
    }
    const std::string needle = lower(want);
    for (const OutputDevice& d : devices) {
        if (lower(d.name).find(needle) != std::string::npos) return d.id;
    }
    NXLOG_W("audio", "xaudio2: [audio] device = %s matches no active output device; using the default device", want.c_str());
    return {};
}

class XAudio2Sink final : public AudioSink {
public:
    ~XAudio2Sink() override { close(); }

    bool open(uint32_t sample_rate, uint32_t channels) override;
    void close() override;
    uint32_t write(const int16_t* pcm, uint32_t frames) override;
    uint32_t queued_frames() override;
    bool has_device_clock() const override { return true; }   // SamplesPlayed is the device's own clock
    const char* name() const override { return "xaudio2"; }
    SinkStats stats() override;

private:
    struct Slot {
        std::vector<int16_t> pcm;   // kSlotFrames * channels values
        uint32_t frames = 0;
        bool busy = false;          // submitted and not yet finished by the voice
    };

    // The voice callback. XAudio2 calls it on its own thread. OnBufferEnd returns a buffer to the pool; OnVoiceProcessingPassStart
    // checks for underruns.
    struct Callback final : IXAudio2VoiceCallback {
        XAudio2Sink* owner = nullptr;
        void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32 bytes_required) noexcept override {
            owner->on_pass_start(bytes_required);
        }
        void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() noexcept override {}
        void STDMETHODCALLTYPE OnStreamEnd() noexcept override {}
        void STDMETHODCALLTYPE OnBufferStart(void*) noexcept override {}
        void STDMETHODCALLTYPE OnBufferEnd(void* ctx) noexcept override {
            owner->release_slot(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(ctx)));
        }
        void STDMETHODCALLTYPE OnLoopEnd(void*) noexcept override {}
        void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) noexcept override {
            NXLOG_W("audio", "xaudio2: voice error 0x%08lx", static_cast<unsigned long>(error));
        }
    };

    // IXAudio2EngineCallback, on XAudio2's thread. Calling back into XAudio2 from here would deadlock, so it only flags the loss.
    struct EngineCallback final : IXAudio2EngineCallback {
        XAudio2Sink* owner = nullptr;
        void STDMETHODCALLTYPE OnProcessingPassStart() noexcept override {}
        void STDMETHODCALLTYPE OnProcessingPassEnd() noexcept override {}
        void STDMETHODCALLTYPE OnCriticalError(HRESULT error) noexcept override { owner->note_lost(error); }
    };

    enum State : int {
        kRunning = 0,   // the device plays (also: not open)
        kLost,          // a loss was flagged; the write thread has not switched yet
        kStandIn,       // silent stand-in; the worker is retrying
        kReady,         // the worker built a new device; the write thread has not adopted it yet
    };

    void note_lost(HRESULT why) noexcept;
    void enter_standin();               // write thread: Lost -> StandIn
    void adopt_device();                // write thread: Ready -> Running
    void recover_main();                // worker thread
    void stop_worker();
    uint32_t standin_write(uint32_t frames);
    bool create_device(bool initial);   // engine, mastering voice, source voice for rate_/channels_; undoes what it made on failure
    void teardown_device();             // voices and engine only
    void release_slot(uint32_t idx);
    void on_pass_start(UINT32 bytes_required) noexcept;
    void note_queued(uint32_t buffers) noexcept;
    uint32_t queued_now() noexcept;                  // submitted frames minus the frames the device has played
    void note_queue_frames(bool playing) noexcept;   // peak and low water marks of queued_now()
    uint32_t submit(const int16_t* pcm, uint32_t frames);
    void teardown();   // teardown_device() and our COM initialisation; safe when nothing is open

    IXAudio2* engine_ = nullptr;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SourceVoice* source_ = nullptr;
    Callback callback_;
    EngineCallback engine_callback_;
    bool engine_registered_ = false;
    bool com_owned_ = false;          // this sink called CoInitializeEx successfully and must balance it
    DWORD com_thread_ = 0;            // the thread that did, so CoUninitialize runs there
    uint32_t channels_ = 0;
    uint32_t rate_ = 48000;
    float applied_volume_ = 1.0f;     // the volume last set on the source voice (write thread)

    std::mutex mu_;                   // guards slots_ (write, queued_frames and the callback)
    std::mutex voice_mu_;             // keeps source_ alive while queued_frames() reads it from another thread (the stats logger);
                                      // never taken on XAudio2's own thread, since DestroyVoice waits for the callbacks
    std::vector<Slot> slots_ = std::vector<Slot>(kSlots);

    // Device recovery. state_ goes Running -> Lost (any thread, CAS) -> StandIn (write thread) -> Ready (worker) -> Running (write thread).
    // While StandIn the worker owns engine_/master_/source_ (it publishes source_ under voice_mu_); the write thread leaves them alone
    // until it sees Ready (acquire) or closes.
    std::atomic<int> state_{kRunning};
    std::mutex standin_mu_;          // guards standin_ (write thread and queued_frames())
    StandInClock standin_;
    std::thread worker_;
    std::mutex worker_mu_;           // guards worker_stop_ for worker_cv_
    std::condition_variable worker_cv_;
    bool worker_stop_ = false;
    std::atomic<uint64_t> device_losses_{0};

    // Counters for stats(). They are not reset by open().
    std::atomic<uint64_t> frames_written_{0};
    std::atomic<uint64_t> frames_accepted_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint32_t> queued_peak_{0};
    std::atomic<uint64_t> submitted_frames_{0};          // frames handed to the voice since open()
    std::atomic<uint32_t> queue_peak_frames_{0};
    std::atomic<uint32_t> queue_low_frames_{0xffffffffu};
    std::atomic<bool> started_{false};   // a buffer has been submitted since open()
    std::atomic<bool> starved_{false};   // the device is out of buffers now; an underrun is counted once per such stretch
};

bool XAudio2Sink::open(uint32_t sample_rate, uint32_t channels) {
    close();
    if (channels != 2 && channels != 6) {
        NXLOG_W("audio", "xaudio2: %u channels not supported (2 or 6)", channels);
        return false;
    }
    if (sample_rate < XAUDIO2_MIN_SAMPLE_RATE || sample_rate > XAUDIO2_MAX_SAMPLE_RATE) {
        NXLOG_W("audio", "xaudio2: sample rate %u out of range", sample_rate);
        return false;
    }

    // XAudio2 wants COM. Initialise it only when this thread has no mode yet; an existing mode (RPC_E_CHANGED_MODE) is kept.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (com == S_OK || com == S_FALSE) {
        com_owned_ = true;
        com_thread_ = GetCurrentThreadId();
    } else if (com != RPC_E_CHANGED_MODE) {
        NXLOG_W("audio", "xaudio2: CoInitializeEx failed (0x%08lx)", static_cast<unsigned long>(com));
        return false;
    }

    channels_ = channels;
    rate_ = sample_rate;
    state_.store(kRunning);
    if (!create_device(true)) {
        teardown();
        return false;
    }
    NXLOG_I("audio", "xaudio2: opened %u Hz, %u channels, volume %.2f", sample_rate, channels, applied_volume_);
    return true;
}

// Builds the engine, the mastering voice on the chosen (or default) device and the started source voice for rate_/channels_. On failure
// everything made here is destroyed again. `initial` is the first open (it lists the devices in the log).
bool XAudio2Sink::create_device(bool initial) {
    const uint32_t sample_rate = rate_;
    const uint32_t channels = channels_;
    HRESULT hr = XAudio2Create(&engine_, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        NXLOG_W("audio", "xaudio2: XAudio2Create failed (0x%08lx)", static_cast<unsigned long>(hr));
        teardown_device();
        return false;
    }
    engine_callback_.owner = this;
    engine_registered_ = SUCCEEDED(engine_->RegisterForCallbacks(&engine_callback_));

    const std::vector<OutputDevice> devices = list_output_devices();
    if (initial) log_devices(devices);
    const std::wstring device_id = pick_device(devices, config_get_str("audio", "device", "default"));
    auto create_master = [this](const wchar_t* id) {
        return engine_->CreateMasteringVoice(&master_, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, id, nullptr,
                                             AudioCategory_GameEffects);
    };
    hr = create_master(device_id.empty() ? nullptr : device_id.c_str());
    if (FAILED(hr) && !device_id.empty()) {
        NXLOG_W("audio", "xaudio2: the chosen output device failed (CreateMasteringVoice 0x%08lx); using the default device",
                static_cast<unsigned long>(hr));
        hr = create_master(nullptr);
    }
    if (FAILED(hr)) {
        NXLOG_W("audio", "xaudio2: no audio output device (CreateMasteringVoice 0x%08lx)", static_cast<unsigned long>(hr));
        teardown_device();
        return false;
    }

    // Stereo is plain PCM; 5.1 needs WAVEFORMATEXTENSIBLE with the channel mask.
    WAVEFORMATEXTENSIBLE wfx{};
    WAVEFORMATEX& f = wfx.Format;
    f.wFormatTag = channels == 2 ? WAVE_FORMAT_PCM : WAVE_FORMAT_EXTENSIBLE;
    f.nChannels = static_cast<WORD>(channels);
    f.nSamplesPerSec = sample_rate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = static_cast<WORD>(channels * 2);
    f.nAvgBytesPerSec = sample_rate * f.nBlockAlign;
    if (channels == 6) {
        f.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wfx.Samples.wValidBitsPerSample = 16;
        wfx.dwChannelMask = kChannelMask51;
        wfx.SubFormat = kSubtypePcm;
    }

    started_.store(false);
    starved_.store(false);
    submitted_frames_.store(0);
    callback_.owner = this;
    IXAudio2SourceVoice* voice = nullptr;
    hr = engine_->CreateSourceVoice(&voice, &wfx.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, &callback_, nullptr, nullptr);
    if (FAILED(hr)) {
        NXLOG_W("audio", "xaudio2: CreateSourceVoice(%u Hz, %u ch) failed (0x%08lx)", sample_rate, channels,
                static_cast<unsigned long>(hr));
        teardown_device();
        return false;
    }
    hr = voice->Start(0);
    if (FAILED(hr)) {
        NXLOG_W("audio", "xaudio2: source voice Start failed (0x%08lx)", static_cast<unsigned long>(hr));
        voice->DestroyVoice();
        teardown_device();
        return false;
    }
    applied_volume_ = effective_volume();
    voice->SetVolume(applied_volume_);

    {
        std::lock_guard<std::mutex> lk(mu_);
        for (Slot& s : slots_) {
            s.pcm.assign(static_cast<size_t>(kSlotFrames) * channels, 0);
            s.frames = 0;
            s.busy = false;
        }
    }
    {
        std::lock_guard<std::mutex> vl(voice_mu_);
        source_ = voice;
    }
    return true;
}

void XAudio2Sink::close() {
    stop_worker();
    const bool recovering = state_.load() != kRunning;
    if (!engine_ && !com_owned_ && !recovering) return;
    if (source_ || recovering) {
        NXLOG_I("audio", "%s", format_stats(name(), stats()).c_str());
        started_.store(false);
        if (source_) {
            source_->Stop(0);
            source_->FlushSourceBuffers();
            // DestroyVoice waits for the audio thread to leave the callbacks.
            std::lock_guard<std::mutex> vl(voice_mu_);
            source_->DestroyVoice();
            source_ = nullptr;
        }
    }
    teardown();
    state_.store(kRunning);
    std::lock_guard<std::mutex> lk(mu_);
    for (Slot& s : slots_) {
        s.frames = 0;
        s.busy = false;
    }
}

void XAudio2Sink::teardown_device() {
    if (source_) {
        std::lock_guard<std::mutex> vl(voice_mu_);
        source_->DestroyVoice();
        source_ = nullptr;
    }
    if (master_) {
        master_->DestroyVoice();
        master_ = nullptr;
    }
    if (engine_) {
        if (engine_registered_) engine_->UnregisterForCallbacks(&engine_callback_);
        engine_registered_ = false;
        engine_->Release();
        engine_ = nullptr;
    }
}

void XAudio2Sink::teardown() {
    teardown_device();
    if (com_owned_) {
        com_owned_ = false;
        if (GetCurrentThreadId() == com_thread_) CoUninitialize();
    }
    channels_ = 0;
}

uint32_t XAudio2Sink::write(const int16_t* pcm, uint32_t frames) {
    int st = state_.load(std::memory_order_acquire);
    if (st == kLost) {
        enter_standin();
        st = state_.load(std::memory_order_acquire);
    }
    if (st == kReady) {
        adopt_device();
        st = state_.load(std::memory_order_acquire);
    }
    const uint32_t accepted = (pcm && frames) ? (st == kRunning ? submit(pcm, frames) : standin_write(frames)) : 0;
    frames_written_.fetch_add(frames, std::memory_order_relaxed);
    frames_accepted_.fetch_add(accepted, std::memory_order_relaxed);
    frames_dropped_.fetch_add(frames - accepted, std::memory_order_relaxed);
    return accepted;
}

uint32_t XAudio2Sink::submit(const int16_t* pcm, uint32_t frames) {
    if (!source_) return 0;
    const float volume = effective_volume();
    if (volume != applied_volume_) {
        source_->SetVolume(volume);
        applied_volume_ = volume;
    }
    const size_t ch = channels_;
    uint32_t accepted = 0;
    while (accepted < frames) {
        XAUDIO2_VOICE_STATE state{};
        source_->GetState(&state);
        if (state.BuffersQueued >= kMaxQueued) break;

        const uint32_t n = std::min<uint32_t>(frames - accepted, kSlotFrames);
        uint32_t idx = kSlots;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (uint32_t i = 0; i < kSlots; i++) {
                if (!slots_[i].busy) {
                    idx = i;
                    break;
                }
            }
            if (idx == kSlots) break;   // pool empty: the voice has not returned buffers yet
            Slot& s = slots_[idx];
            s.busy = true;
            s.frames = n;
            std::memcpy(s.pcm.data(), pcm + static_cast<size_t>(accepted) * ch, static_cast<size_t>(n) * ch * sizeof(int16_t));
        }

        XAUDIO2_BUFFER buf{};
        buf.AudioBytes = n * static_cast<UINT32>(ch) * 2;
        buf.pAudioData = reinterpret_cast<const BYTE*>(slots_[idx].pcm.data());
        buf.pContext = reinterpret_cast<void*>(static_cast<uintptr_t>(idx));
        const HRESULT hr = source_->SubmitSourceBuffer(&buf);
        if (FAILED(hr)) {
            release_slot(idx);
            if (hr == XAUDIO2_E_DEVICE_INVALIDATED) note_lost(hr);
            break;
        }
        started_.store(true);
        starved_.store(false);
        submitted_frames_.fetch_add(n, std::memory_order_relaxed);
        note_queued(state.BuffersQueued + 1);
        note_queue_frames(false);
        accepted += n;
    }
    return accepted;
}

// Runs on XAudio2's audio thread. Cheap: one GetState and a few atomic updates.
void XAudio2Sink::on_pass_start(UINT32 bytes_required) noexcept {
    if (bytes_required == 0 || !started_.load() || !source_) return;
    XAUDIO2_VOICE_STATE state{};
    source_->GetState(&state);
    note_queued(state.BuffersQueued);
    note_queue_frames(true);
    if (state.BuffersQueued == 0) {
        if (!starved_.exchange(true)) underruns_.fetch_add(1, std::memory_order_relaxed);
    } else {
        starved_.store(false);
    }
}

void XAudio2Sink::note_queued(uint32_t buffers) noexcept {
    uint32_t peak = queued_peak_.load(std::memory_order_relaxed);
    while (buffers > peak && !queued_peak_.compare_exchange_weak(peak, buffers, std::memory_order_relaxed)) {
    }
}

// The device's play position is SamplesPlayed of the source voice: it counts the samples the voice has consumed, so submitted minus played
// is exact (not rounded to whole buffers). A voice that starved keeps counting only what it really played.
uint32_t XAudio2Sink::queued_now() noexcept {
    IXAudio2SourceVoice* v = source_;
    if (!v) return 0;
    XAUDIO2_VOICE_STATE state{};
    v->GetState(&state);
    const uint64_t submitted = submitted_frames_.load(std::memory_order_relaxed);
    return submitted > state.SamplesPlayed ? static_cast<uint32_t>(submitted - state.SamplesPlayed) : 0u;
}

void XAudio2Sink::note_queue_frames(bool playing) noexcept {
    const uint32_t q = queued_now();
    uint32_t peak = queue_peak_frames_.load(std::memory_order_relaxed);
    while (q > peak && !queue_peak_frames_.compare_exchange_weak(peak, q, std::memory_order_relaxed)) {
    }
    if (playing && q > 0 && submitted_frames_.load(std::memory_order_relaxed) > rate_ / 2 + q) {   // not the start-up fill: after half a second played
        uint32_t low = queue_low_frames_.load(std::memory_order_relaxed);
        while (q < low && !queue_low_frames_.compare_exchange_weak(low, q, std::memory_order_relaxed)) {
        }
    }
}

uint32_t XAudio2Sink::queued_frames() {
    const int st = state_.load(std::memory_order_acquire);
    if (st == kLost) return 0;   // the dead device drains nothing; write() switches to the stand-in on the renderer's next frame
    if (st == kStandIn || st == kReady) {
        std::lock_guard<std::mutex> sl(standin_mu_);
        return standin_.queued(now_seconds());
    }
    {
        std::lock_guard<std::mutex> vl(voice_mu_);
        if (source_) return queued_now();
    }
    std::lock_guard<std::mutex> lk(mu_);
    uint32_t total = 0;
    for (const Slot& s : slots_) {
        if (s.busy) total += s.frames;
    }
    return total;
}

SinkStats XAudio2Sink::stats() {
    SinkStats s;
    s.frames_written = frames_written_.load(std::memory_order_relaxed);
    s.frames_accepted = frames_accepted_.load(std::memory_order_relaxed);
    s.frames_dropped = frames_dropped_.load(std::memory_order_relaxed);
    s.underruns = underruns_.load(std::memory_order_relaxed);
    s.queued_peak = queued_peak_.load(std::memory_order_relaxed);
    s.queued_peak_frames = queue_peak_frames_.load(std::memory_order_relaxed);
    const uint32_t low = queue_low_frames_.load(std::memory_order_relaxed);
    s.queued_low_frames = low == 0xffffffffu ? 0 : low;
    s.sample_rate = rate_;
    return s;
}

void XAudio2Sink::note_lost(HRESULT why) noexcept {
    int expected = kRunning;
    if (state_.compare_exchange_strong(expected, kLost, std::memory_order_acq_rel)) {
        device_losses_.fetch_add(1, std::memory_order_relaxed);
        NXLOG_W("audio", "xaudio2: output device lost (0x%08lx)", static_cast<unsigned long>(why));
    }
}

uint32_t XAudio2Sink::standin_write(uint32_t frames) {
    std::lock_guard<std::mutex> sl(standin_mu_);
    return standin_.write(frames, now_seconds());
}

// Write thread. The voices belong to us here: the worker is not running yet and queued_frames() reads source_ only under voice_mu_.
void XAudio2Sink::enter_standin() {
    started_.store(false);
    teardown_device();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (Slot& s : slots_) {
            s.frames = 0;
            s.busy = false;
        }
    }
    {
        std::lock_guard<std::mutex> sl(standin_mu_);
        standin_.begin(rate_, now_seconds());
    }
    if (worker_.joinable()) worker_.join();   // a finished worker from an earlier recovery
    {
        std::lock_guard<std::mutex> wl(worker_mu_);
        worker_stop_ = false;
    }
    state_.store(kStandIn, std::memory_order_release);
    NXLOG_W("audio", "xaudio2: playing on a silent stand-in until an output device can be opened again");
    worker_ = std::thread([this] { recover_main(); });
}

// Worker thread: tries create_device() with growing pauses until it works or close() stops it.
void XAudio2Sink::recover_main() {
    log_set_thread_name("audio-recover");
    unsigned failures = 0;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(worker_mu_);
            if (worker_cv_.wait_for(lk, std::chrono::duration<double>(reopen_delay_s(failures)), [this] { return worker_stop_; })) return;
        }
        if (create_device(false)) {
            std::lock_guard<std::mutex> lk(worker_mu_);
            if (worker_stop_) return;   // close() tears down what was just built
            state_.store(kReady, std::memory_order_release);
            return;
        }
        failures++;
        if (failures <= 3 || failures % 12 == 0) {
            NXLOG_W("audio", "xaudio2: output device still unavailable (attempt %u)", failures);
        }
    }
}

void XAudio2Sink::adopt_device() {
    if (worker_.joinable()) worker_.join();   // it has returned: Ready is its last act
    state_.store(kRunning, std::memory_order_release);
    NXLOG_I("audio", "xaudio2: output device reopened (%u Hz, %u channels, volume %.2f)", rate_, channels_, applied_volume_);
}

void XAudio2Sink::stop_worker() {
    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        worker_stop_ = true;
    }
    worker_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void XAudio2Sink::release_slot(uint32_t idx) {
    std::lock_guard<std::mutex> lk(mu_);
    if (idx >= kSlots) return;
    slots_[idx].busy = false;
    slots_[idx].frames = 0;
}

}  // namespace

std::shared_ptr<AudioSink> make_xaudio2_sink() {
    // The sink is only useful when the system XAudio2 DLL is there; the device is opened later in open().
    HMODULE lib = LoadLibraryExW(L"xaudio2_9.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) return nullptr;
    FreeLibrary(lib);
    return std::make_shared<XAudio2Sink>();
}

}  // namespace nxrt::audio
