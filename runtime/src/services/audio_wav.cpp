// SPDX-License-Identifier: GPL-3.0-or-later
// 16-bit PCM WAV writer (WavWriter) and the tee sink behind [audio] wav_dump (include/nxrt/audio_sink.h).
// The header is written with zero sizes, then patched: at the first write() that brings the data past another second of audio and
// at close(). Every patch flushes the data before it, so the sizes in the header never claim bytes that are not in the file.
#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "nxrt/audio_sink.h"
#include "nxrt/log.h"

namespace nxrt::audio {
namespace {

// The RIFF size is 32-bit: stop before the data would not fit (the header is at most 68 bytes).
constexpr uint64_t kMaxDataBytes = 0xFFFFFFFFull - 64;

// KSDATAFORMAT_SUBTYPE_PCM {00000001-0000-0010-8000-00aa00389b71} as it is stored in the file (little-endian first three fields).
constexpr uint8_t kSubtypePcm[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

// Little-endian byte builder for the header. The sample data is written as the host stores it (x64 is little-endian).
class ByteBuf {
public:
    void raw(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        bytes_.insert(bytes_.end(), b, b + n);
    }
    void u16(uint32_t v) {
        bytes_.push_back(static_cast<uint8_t>(v));
        bytes_.push_back(static_cast<uint8_t>(v >> 8));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; i++) bytes_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    const uint8_t* data() const { return bytes_.data(); }
    size_t size() const { return bytes_.size(); }

private:
    std::vector<uint8_t> bytes_;
};

// Tee: forwards every call to the inner sink and writes the frames that go to write() into a WAV file. The file is written by a thread of
// its own: write() runs on the audio renderer's thread, and a disk that stalls for a while (a busy machine, a full write cache) must not
// make the renderer miss its frame. write() only copies the frames into a queue; close() waits until the queue is on disk.
class WavTeeSink final : public AudioSink {
public:
    WavTeeSink(std::shared_ptr<AudioSink> inner, std::string path)
        : inner_(std::move(inner)), path_(std::move(path)), name_(std::string(inner_->name()) + "+wav") {
        thread_ = std::thread([this] { run(); });
    }
    ~WavTeeSink() override {
        close();
        {
            std::lock_guard<std::mutex> lk(mu_);
            quit_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    bool open(uint32_t rate, uint32_t channels) override {
        const bool ok = inner_->open(rate, channels);
        // The file is kept while the format stays the same; a different format (or a reopen after close) starts a numbered file.
        if (!file_open_ || rate_ != rate || channels_ != channels) {
            Job j;
            j.kind = Job::Close;
            push(std::move(j));
            Job o;
            o.kind = Job::Open;
            o.path = file_for(next_index_++);
            o.rate = rate;
            o.channels = channels;
            push(std::move(o));
            file_open_ = true;
            rate_ = rate;
            channels_ = channels;
        }
        return ok;
    }

    void close() override {
        inner_->close();
        Job j;
        j.kind = Job::Close;
        push(std::move(j));
        file_open_ = false;
        drain();
        NXLOG_I("audio", "%s", format_stats(name_.c_str(), stats()).c_str());
    }

    uint32_t write(const int16_t* pcm, uint32_t frames) override {
        frames_written_ += frames;
        if (file_open_ && channels_ != 0) {   // every frame, whether or not the inner sink queues it
            Job j;
            j.kind = Job::Data;
            j.pcm.assign(pcm, pcm + static_cast<size_t>(frames) * channels_);
            push(std::move(j));
        }
        return inner_->write(pcm, frames);
    }

    uint32_t queued_frames() override { return inner_->queued_frames(); }
    bool has_device_clock() const override { return inner_->has_device_clock(); }
    const char* name() const override { return name_.c_str(); }

    SinkStats stats() override {
        SinkStats s = inner_->stats();
        s.frames_written = frames_written_;
        return s;
    }

private:
    struct Job {
        enum Kind { Open, Data, Close } kind = Data;
        std::string path;
        uint32_t rate = 0, channels = 0;
        std::vector<int16_t> pcm;
    };

    void push(Job&& j) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.push_back(std::move(j));
        }
        cv_.notify_one();
    }

    // Waits until the writer thread has handled everything queued so far.
    void drain() {
        std::unique_lock<std::mutex> lk(mu_);
        idle_cv_.wait(lk, [this] { return queue_.empty() && !busy_; });
    }

    void run() {
        nxrt::log_set_thread_name("audio-wav");
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return quit_ || !queue_.empty(); });
                if (queue_.empty()) return;   // quit_ and nothing left
                j = std::move(queue_.front());
                queue_.pop_front();
                busy_ = true;
            }
            switch (j.kind) {
                case Job::Open:
                    if (wav_.open(j.path, j.rate, j.channels)) {
                        NXLOG_I("audio", "wav_dump: writing %s (%u Hz, %u channels)", j.path.c_str(), j.rate, j.channels);
                    } else {
                        NXLOG_W("audio", "wav_dump: cannot create %s", j.path.c_str());
                    }
                    break;
                case Job::Data:
                    wav_.write(j.pcm.data(), static_cast<uint32_t>(j.pcm.size() / std::max<uint32_t>(wav_.channels(), 1)));
                    break;
                case Job::Close:
                    wav_.close();
                    break;
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                busy_ = false;
            }
            idle_cv_.notify_all();
        }
    }

    // Index 0 is the configured path; index n >= 1 is <stem>.<n><ext> next to it.
    std::string file_for(uint32_t index) const {
        if (index == 0) return path_;
        const std::filesystem::path p(path_);
        return (p.parent_path() / (p.stem().string() + "." + std::to_string(index) + p.extension().string())).string();
    }

    std::shared_ptr<AudioSink> inner_;
    std::string path_;
    std::string name_;
    // renderer thread only
    bool file_open_ = false;
    uint32_t rate_ = 0, channels_ = 0;
    uint32_t next_index_ = 0;
    uint64_t frames_written_ = 0;
    // writer thread
    WavWriter wav_;
    std::mutex mu_;
    std::condition_variable cv_, idle_cv_;
    std::deque<Job> queue_;
    bool quit_ = false, busy_ = false;
    std::thread thread_;
};

}  // namespace

WavWriter::~WavWriter() { close(); }

bool WavWriter::open(const std::string& path, uint32_t sample_rate, uint32_t channels) {
    close();
    if (sample_rate == 0 || channels == 0) return false;
    file_ = _wfopen(std::filesystem::path(path).c_str(), L"wb+");
    if (!file_) return false;

    sample_rate_ = sample_rate;
    channels_ = channels;
    frames_ = 0;
    data_bytes_ = 0;
    bytes_at_patch_ = 0;
    const uint32_t block = channels * 2;
    bytes_per_second_ = static_cast<uint64_t>(sample_rate) * block;

    const bool extensible = channels == 6;
    ByteBuf h;
    h.raw("RIFF", 4);
    h.u32(0);                                   // patched
    h.raw("WAVE", 4);
    h.raw("fmt ", 4);
    h.u32(extensible ? 40 : 16);
    h.u16(extensible ? 0xFFFE : 1);             // WAVE_FORMAT_EXTENSIBLE or WAVE_FORMAT_PCM
    h.u16(channels);
    h.u32(sample_rate);
    h.u32(sample_rate * block);                 // average bytes per second
    h.u16(block);
    h.u16(16);                                  // bits per sample
    if (extensible) {
        h.u16(22);                              // cbSize
        h.u16(16);                              // valid bits per sample
        h.u32(0x3F);                            // channel mask: 5.1
        h.raw(kSubtypePcm, sizeof kSubtypePcm);
    }
    h.raw("data", 4);
    h.u32(0);                                   // patched
    header_size_ = h.size();

    if (std::fwrite(h.data(), 1, h.size(), file_) != h.size()) {
        close();
        return false;
    }
    return true;
}

void WavWriter::write(const int16_t* pcm, uint32_t frames) {
    if (!file_ || !pcm || frames == 0) return;
    const uint64_t block = static_cast<uint64_t>(channels_) * 2;
    const uint64_t room = (kMaxDataBytes - data_bytes_) / block;
    if (frames > room) frames = static_cast<uint32_t>(room);
    if (frames == 0) return;

    const size_t values = static_cast<size_t>(frames) * channels_;
    if (std::fwrite(pcm, sizeof(int16_t), values, file_) != values) return;
    data_bytes_ += frames * block;
    frames_ += frames;
    if (data_bytes_ - bytes_at_patch_ >= bytes_per_second_) patch_header();
}

void WavWriter::patch_header() {
    if (!file_) return;
    ByteBuf riff;
    riff.u32(static_cast<uint32_t>(header_size_ - 8 + data_bytes_));
    ByteBuf data;
    data.u32(static_cast<uint32_t>(data_bytes_));
    std::fseek(file_, 4, SEEK_SET);
    std::fwrite(riff.data(), 1, 4, file_);
    std::fseek(file_, static_cast<long>(header_size_ - 4), SEEK_SET);
    std::fwrite(data.data(), 1, 4, file_);
    std::fflush(file_);
    std::fseek(file_, 0, SEEK_END);
    bytes_at_patch_ = data_bytes_;
}

void WavWriter::close() {
    if (!file_) return;
    patch_header();
    std::fclose(file_);
    file_ = nullptr;
}

std::shared_ptr<AudioSink> make_wav_tee(std::shared_ptr<AudioSink> inner, const std::string& path) {
    if (!inner) inner = std::make_shared<NullSink>();
    return std::make_shared<WavTeeSink>(std::move(inner), path);
}

}  // namespace nxrt::audio
