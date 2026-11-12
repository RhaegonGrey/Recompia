// SPDX-License-Identifier: GPL-3.0-or-later
// Host-folder volumes for fsp-srv; see fs_volume.h for the model. The journal idea (committed folder, copy-on-write work folder,
// tombstones, commit decided by one rename) is from smo-recomp runtime/fs/nnfs.c (GPL-3.0); the code is new.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include "fs_volume.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "nxrt/log.h"
#include "nxrt/services_api.h"
#include "nxrt/write_watch.h"

namespace stdfs = std::filesystem;

namespace nxrt::services::fs {

namespace {

std::atomic<int> g_crash_point{0};

// Test hook (set_io_failure_for_tests): the `g_fail_nth`-th host operation at `g_fail_point` fails with `g_fail_err`.
std::atomic<int> g_fail_point{0}, g_fail_nth{0};
std::atomic<uint32_t> g_fail_err{0};
bool injected_failure(Volume::IoPoint point, DWORD* err) {
    if (g_fail_point.load() != static_cast<int>(point) || g_fail_nth.load() <= 0) return false;
    if (g_fail_nth.fetch_sub(1) != 1) return false;
    *err = g_fail_err.load();
    NXLOG_W("fs", "test: injected host failure %lu at point %d", static_cast<unsigned long>(*err), static_cast<int>(point));
    return true;
}

// Refused save commits, for the player notice (services_api.h fsapi::save_failures).
std::atomic<uint32_t> g_save_fail_count{0}, g_save_fail_err{0};
void report_save_failure(DWORD err) {
    g_save_fail_err.store(err);
    g_save_fail_count.fetch_add(1);
}

bool disk_full(DWORD e) { return e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL; }
// The guest result of a failed host operation: "usable space not enough" for a full disk, a generic failure otherwise.
uint32_t host_rc(DWORD e) { return disk_full(e) ? rc::kUsableSpaceNotEnough : rc::kGenericFailure; }
// FlushFileBuffers results that mean "this file system does not flush" (Wine maps an fsync EINVAL to one of these): the data written is
// in the cache and not suspect, so they neither fail a flush nor taint a save.
bool flush_unsupported(DWORD e) { return e == ERROR_INVALID_FUNCTION || e == ERROR_NOT_SUPPORTED || e == ERROR_INVALID_PARAMETER; }

// The first host failure while changing a journalled volume since its last commit (Win32 error, 0 = none). The game does not check the
// results of Write / SetSize and commits anyway, so a full disk or a file held by an antivirus or OneDrive would otherwise commit a
// truncated or torn file over the last good save. commit() refuses a tainted transaction and drops its work folder. The policy is
// pokopia-native's (MIT), windows/src/nxrt/hle/hle_fs.c @7c3655d (save_poison_txn, the refusal in commit_mount_locked); the code is new.
struct Taint {
    std::atomic<uint32_t> err{0};
    void set(DWORD e) {
        uint32_t z = 0;
        if (err.compare_exchange_strong(z, e ? e : ERROR_GEN_FAILURE))
            NXLOG_E("fs", "save write failed (host error %lu): the next commit of this save is refused", static_cast<unsigned long>(e));
    }
};

stdfs::path u8(const std::string& s) { return stdfs::u8path(s); }
std::string to_u8(const stdfs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

std::string rel_of(const std::string& path) { return path.size() > 1 ? path.substr(1) : std::string(); }

std::string parent_of(const std::string& path) {
    const size_t i = path.rfind('/');
    return i == 0 || i == std::string::npos ? std::string("/") : path.substr(0, i);
}

// Positioned I/O on a Win32 handle (several threads may call it at once).
class HostFile final : public VFile {
public:
    HostFile(HANDLE h, uint32_t mode, std::shared_ptr<Taint> taint) : h_(h), mode_(mode), taint_(std::move(taint)) {}
    ~HostFile() override {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    uint32_t read(int64_t offset, void* dst, uint64_t size, uint64_t* got) override {
        *got = 0;
        if (offset < 0) return rc::kOutOfRange;
        if (!(mode_ & kModeRead)) return rc::kReadNotPermitted;
        uint8_t* d = static_cast<uint8_t*>(dst);
        const mem::ww::OsWriteScope ws(d, size);   // ReadFile fails on a page the write tracker made read-only
        while (size) {
            const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(size, 0x40000000));
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(static_cast<uint64_t>(offset) & 0xFFFFFFFFu);
            ov.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);
            DWORD n = 0;
            if (!ReadFile(h_, d, chunk, &n, &ov)) {
                if (GetLastError() == ERROR_HANDLE_EOF) break;
                return rc::kGenericFailure;
            }
            if (n == 0) break;
            *got += n;
            d += n;
            offset += n;
            size -= n;
            if (n < chunk) break;
        }
        return 0;
    }
    uint32_t write(int64_t offset, const void* src, uint64_t size) override {
        if (offset < 0) return rc::kOutOfRange;
        if (!(mode_ & kModeWrite)) return rc::kWriteNotPermitted;
        int64_t cur = 0;
        get_size(&cur);
        if (static_cast<uint64_t>(offset) + size > static_cast<uint64_t>(cur) && !(mode_ & kModeAppend)) return rc::kNoAppend;
        const uint8_t* s = static_cast<const uint8_t*>(src);
        const mem::ww::OsReadScope rs(s, size);   // WriteFile fails on a page the write tracker guarded (a deferred GPU result)
        while (size) {
            const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(size, 0x40000000));
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(static_cast<uint64_t>(offset) & 0xFFFFFFFFu);
            ov.OffsetHigh = static_cast<DWORD>(static_cast<uint64_t>(offset) >> 32);
            DWORD n = 0, e = 0;
            if (injected_failure(Volume::IoPoint::kWrite, &e)) return failed(e);
            if (!WriteFile(h_, s, chunk, &n, &ov)) return failed(GetLastError());
            if (n == 0) return failed(ERROR_DISK_FULL);
            s += n;
            offset += n;
            size -= n;
        }
        return 0;
    }
    uint32_t flush() override {
        if (!(mode_ & kModeWrite)) return 0;
        DWORD e = 0;
        if (!injected_failure(Volume::IoPoint::kFlush, &e)) {
            if (FlushFileBuffers(h_)) return 0;
            e = GetLastError();
        }
        return flush_unsupported(e) ? 0 : failed(e);
    }
    uint32_t set_size(int64_t size) override {
        if (!(mode_ & kModeWrite)) return rc::kWriteNotPermitted;
        if (size < 0) return rc::kOutOfRange;
        std::lock_guard<std::mutex> lk(size_mu_);
        LARGE_INTEGER li;
        li.QuadPart = size;
        DWORD e = 0;
        if (injected_failure(Volume::IoPoint::kSetSize, &e)) return failed(e);
        if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN) || !SetEndOfFile(h_)) return failed(GetLastError());
        return 0;
    }
    uint32_t get_size(int64_t* size) override {
        LARGE_INTEGER li;
        if (!GetFileSizeEx(h_, &li)) return rc::kGenericFailure;
        *size = li.QuadPart;
        return 0;
    }

private:
    uint32_t failed(DWORD e) {
        if (taint_) taint_->set(e);
        return host_rc(e);
    }
    HANDLE h_;
    uint32_t mode_;
    std::shared_ptr<Taint> taint_;   // the save transaction of a write handle on a journalled volume (null otherwise)
    std::mutex size_mu_;
};

HANDLE open_host(const stdfs::path& p, bool write, DWORD disposition) {
    return CreateFileW(p.c_str(), write ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
}

bool pid_alive(DWORD pid) {
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!p) return GetLastError() == ERROR_ACCESS_DENIED;
    const bool alive = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    CloseHandle(p);
    return alive;
}

// A mutex shared by every process that opens the same save folder: commit and recovery do not interleave.
class RootLock {
public:
    explicit RootLock(const std::string& root) {
        size_t h = std::hash<std::string>{}(root);
        wchar_t name[64];
        std::swprintf(name, 64, L"nxrt_save_%016llx", static_cast<unsigned long long>(h));
        m_ = CreateMutexW(nullptr, FALSE, name);
        if (m_) WaitForSingleObject(m_, INFINITE);
    }
    ~RootLock() {
        if (m_) {
            ReleaseMutex(m_);
            CloseHandle(m_);
        }
    }

private:
    HANDLE m_ = nullptr;
};

// Applies a decided commit. Marker lines: "W <work folder>", "R <committed folder>", then "D rel" (delete), "M rel" (make folder),
// "F rel" (move file work -> committed). Every step can be repeated, so a commit interrupted here is finished by running it again.
// Returns the number of steps, -1 when the marker cannot be read. `stop_after` (> 0) stops after that many steps (crash simulation).
int apply_marker(const stdfs::path& marker, int stop_after = 0) {
    std::ifstream in(marker, std::ios::binary);
    if (!in) return -1;
    std::string line;
    stdfs::path work, root;
    int n = 0;
    std::error_code ec;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.size() < 3) continue;
        const std::string arg = line.substr(2);
        if (line[0] == 'W') work = u8(arg);
        else if (line[0] == 'R') root = u8(arg);
        else if (work.empty() || root.empty()) continue;
        else if (line[0] == 'D') {
            stdfs::remove_all(root / u8(arg), ec);
            n++;
        } else if (line[0] == 'M') {
            stdfs::create_directories(root / u8(arg), ec);
            n++;
        } else if (line[0] == 'F') {
            const stdfs::path a = work / u8(arg), b = root / u8(arg);
            if (stdfs::exists(a, ec)) {
                stdfs::create_directories(b.parent_path(), ec);
                // An antivirus scan or a sync client (OneDrive) may hold the committed file for a moment: a few short retries.
                for (int tries = 0; tries < 6; ++tries) {
                    if (tries) Sleep(50 * tries);
                    stdfs::rename(a, b, ec);
                    if (!ec) break;
                }
                if (ec) {
                    NXLOG_E("fs", "commit: moving %s failed: %s", arg.c_str(), ec.message().c_str());
                    return -1;
                }
            }
            n++;
        } else {
            continue;
        }
        if (stop_after > 0 && n >= stop_after) return n;
    }
    in.close();
    stdfs::remove(marker, ec);
    return n;
}

}  // namespace

// ---- path hygiene ----------------------------------------------------------------------------------------------------------------

bool normalize_path(const char* raw, size_t max_len, std::string* out, uint32_t* err) {
    out->clear();
    if (!raw) {
        *err = rc::kInvalidArgument;
        return false;
    }
    size_t n = 0;
    while (n < max_len && raw[n]) n++;
    if (n == max_len) {   // no terminator inside the buffer
        *err = rc::kTooLongPath;
        return false;
    }
    if (n > 0x300) {
        *err = rc::kTooLongPath;
        return false;
    }
    if (n == 0 || raw[0] != '/') {
        *err = rc::kInvalidArgument;
        return false;
    }
    std::string cur;
    std::vector<std::string> parts;
    for (size_t i = 0; i <= n; ++i) {
        const char c = i < n ? raw[i] : '/';
        if (c == '/') {
            if (cur == "." || cur == "..") {
                *err = rc::kInvalidArgument;
                return false;
            }
            if (!cur.empty()) parts.push_back(cur);
            cur.clear();
        } else if (c == '\\' || static_cast<unsigned char>(c) < 0x20 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            *err = rc::kInvalidArgument;
            return false;
        } else {
            cur += c;
        }
    }
    for (const std::string& p : parts) {
        *out += "/";
        *out += p;
    }
    if (out->empty()) *out = "/";
    return true;
}

// ---- Volume ----------------------------------------------------------------------------------------------------------------------

struct Volume::Impl {
    stdfs::path root, work, marker, journal_file;
    std::mutex mu;
    std::set<std::string> tomb;   // committed paths deleted since the last commit (journalled volumes)
    std::shared_ptr<Taint> taint = std::make_shared<Taint>();   // shared with the write handles: they may outlive the volume
    bool journal = false, ro = false, ok = false;

    // A host failure while changing a journalled volume taints its transaction (see Taint). Returns the guest result.
    uint32_t host_fail(DWORD e) {
        if (journal) {
            taint->set(e);
        }
        return host_rc(e);
    }
    uint32_t host_fail(const std::error_code& ec) { return host_fail(ec.value() ? static_cast<DWORD>(ec.value()) : ERROR_GEN_FAILURE); }

    // Drops the uncommitted changes: the visible tree is the committed one again. False when files are left in the work folder (a handle
    // without FILE_SHARE_DELETE, e.g. an antivirus scan, holds them).
    bool discard_work() {
        std::error_code ec;
        tomb.clear();
        stdfs::remove_all(work, ec);
        stdfs::create_directories(work, ec);
        return stdfs::is_directory(work, ec) && stdfs::is_empty(work, ec);
    }

    bool direct() const { return !journal; }
    stdfs::path cpath(const std::string& path) const { return root / u8(rel_of(path)); }
    stdfs::path wpath(const std::string& path) const { return (journal ? work : root) / u8(rel_of(path)); }

    // The committed layer shows `path` unless it or one of its parents was deleted since the last commit.
    bool committed_visible(const std::string& path) const {
        if (!journal) return false;   // direct volumes have one layer (wpath == cpath)
        for (size_t i = 1; i <= path.size(); ++i)
            if (i == path.size() || path[i] == '/')
                if (tomb.count(path.substr(0, i))) return false;
        std::error_code ec;
        return stdfs::exists(cpath(path), ec);
    }

    struct Found {
        bool exists = false, is_dir = false, in_work = false;
        stdfs::path host;
    };
    Found lookup(const std::string& path) const {
        Found f;
        std::error_code ec;
        if (path == "/") {
            f.exists = f.is_dir = f.in_work = true;
            f.host = wpath(path);
            return f;
        }
        const stdfs::path w = wpath(path);
        if (stdfs::exists(w, ec)) {
            f.exists = f.in_work = true;
            f.is_dir = stdfs::is_directory(w, ec);
            f.host = w;
            return f;
        }
        if (committed_visible(path)) {
            const stdfs::path c = cpath(path);
            f.exists = true;
            f.is_dir = stdfs::is_directory(c, ec);
            f.host = c;
        }
        return f;
    }

    // The parents of `path` exist as directories (in some layer); creates them in the work layer.
    uint32_t ensure_parents(const std::string& path) {
        const std::string par = parent_of(path);
        const Found f = lookup(par);
        if (!f.exists || !f.is_dir) return rc::kPathNotFound;
        std::error_code ec;
        stdfs::create_directories(wpath(par), ec);
        return ec ? host_fail(ec) : 0;
    }

    // Makes sure a committed file also exists in the work layer (copy on write).
    uint32_t materialize_file(const std::string& path) {
        std::error_code ec;
        if (!journal || stdfs::exists(wpath(path), ec)) return 0;
        if (!committed_visible(path)) return rc::kPathNotFound;
        if (const uint32_t r = ensure_parents(path)) return r;
        stdfs::copy_file(cpath(path), wpath(path), stdfs::copy_options::overwrite_existing, ec);
        if (!ec) return 0;
        std::error_code e2;
        stdfs::remove(wpath(path), e2);   // a partial copy must not stand in for the committed file
        return host_fail(ec);
    }

    // Copies the visible committed subtree under `path` into the work layer (before a directory rename).
    uint32_t materialize_tree(const std::string& path) {
        std::vector<DirItem> items;
        if (const uint32_t r = list(path, &items)) return r;
        std::error_code ec;
        stdfs::create_directories(wpath(path), ec);
        for (const DirItem& it : items) {
            const std::string child = (path == "/" ? "" : path) + "/" + it.name;
            if (it.is_dir) {
                if (const uint32_t r = materialize_tree(child)) return r;
            } else if (const uint32_t r = materialize_file(child)) {
                return r;
            }
        }
        return 0;
    }

    uint32_t list(const std::string& path, std::vector<DirItem>* out) {
        const Found f = lookup(path);
        if (!f.exists || !f.is_dir) return rc::kPathNotFound;
        std::map<std::string, DirItem> m;
        std::error_code ec;
        auto scan = [&](const stdfs::path& dir, bool committed) {
            for (stdfs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                DirItem d;
                d.name = to_u8(it->path().filename());
                const std::string child = (path == "/" ? "" : path) + "/" + d.name;
                if (committed && tomb.count(child)) continue;
                std::error_code e2;
                d.is_dir = it->is_directory(e2);
                d.size = d.is_dir ? 0 : static_cast<int64_t>(it->file_size(e2));
                if (committed && m.count(d.name)) continue;
                m[d.name] = d;
            }
        };
        if (stdfs::exists(wpath(path), ec)) scan(wpath(path), false);
        if (journal && committed_visible(path)) scan(cpath(path), true);
        out->clear();
        for (auto& kv : m) out->push_back(kv.second);
        std::sort(out->begin(), out->end(), [](const DirItem& a, const DirItem& b) { return a.name < b.name; });
        return 0;
    }

    uint64_t used(const std::string& path) {
        std::vector<DirItem> items;
        if (list(path, &items)) return 0;
        uint64_t total = 0;
        for (const DirItem& it : items) {
            const std::string child = (path == "/" ? "" : path) + "/" + it.name;
            total += it.is_dir ? used(child) : static_cast<uint64_t>(it.size);
        }
        return total;
    }
};

Volume::Volume(const std::string& root, bool journal, bool read_only, uint64_t capacity)
    : p_(std::make_unique<Impl>()), capacity_(capacity), journal_(journal), read_only_(read_only) {
    Impl& s = *p_;
    s.root = u8(root);
    s.journal = journal;
    s.ro = read_only;
    std::error_code ec;
    if (journal) {
        stdfs::create_directories(s.root, ec);
        recover(root);
        s.marker = stdfs::path(s.root.string() + ".commit");
        // Per instance, not per process: a second Volume over the same save (a read-only open is not cached) used to wipe the
        // first one's work folder while its files were open, and every later write failed with ENOENT (2026-11-10).
        static std::atomic<uint32_t> seq{1};
        const std::string inst = std::to_string(GetCurrentProcessId()) + "." + std::to_string(seq.fetch_add(1, std::memory_order_relaxed));
        s.work = stdfs::path(s.root.string() + ".work." + inst);
        s.journal_file = stdfs::path(s.root.string() + ".journal." + inst);
        stdfs::remove_all(s.work, ec);   // a fresh open starts from the committed state
        stdfs::create_directories(s.work, ec);
        s.ok = stdfs::is_directory(s.root, ec) && stdfs::is_directory(s.work, ec);
    } else {
        s.ok = stdfs::is_directory(s.root, ec);
        if (!s.ok && !read_only) {
            stdfs::create_directories(s.root, ec);
            s.ok = stdfs::is_directory(s.root, ec);
        }
    }
}

Volume::~Volume() {
    if (!p_ || !p_->journal) return;
    std::error_code ec;
    // A decided commit that was not finished keeps its work folder: the next open rolls it forward. Otherwise the uncommitted
    // changes are dropped, as they are when a save is unmounted on the console without Commit.
    if (!stdfs::exists(p_->marker, ec)) stdfs::remove_all(p_->work, ec);
}

bool Volume::ok() const { return p_->ok; }

void Volume::set_crash_point_for_tests(int point) { g_crash_point = point; }

void Volume::set_io_failure_for_tests(IoPoint point, int nth, uint32_t win32_error) {
    g_fail_err = win32_error;
    g_fail_nth = nth;
    g_fail_point = static_cast<int>(point);
}

void Volume::recover(const std::string& root_s) {
    const stdfs::path root = u8(root_s);
    RootLock lock(root_s);
    std::error_code ec;
    const stdfs::path marker(root.string() + ".commit");
    if (stdfs::exists(marker, ec)) {
        const int n = apply_marker(marker);
        NXLOG_W("fs", "a commit of %s was interrupted after it was decided: finished now (%d changes)", root_s.c_str(), n);
    }
    // Work folders and journals of processes that no longer run.
    const stdfs::path dir = root.parent_path();
    const std::string stem = root.filename().string();
    for (stdfs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.compare(0, stem.size(), stem) != 0) continue;
        const std::string tail = name.substr(stem.size());
        const bool is_work = tail.rfind(".work.", 0) == 0, is_journal = tail.rfind(".journal.", 0) == 0;
        if (!is_work && !is_journal) continue;
        const DWORD pid = static_cast<DWORD>(std::strtoul(tail.c_str() + (is_work ? 6 : 9), nullptr, 10));
        if (!pid || pid == GetCurrentProcessId() || pid_alive(pid)) continue;
        std::error_code e2;
        stdfs::remove_all(it->path(), e2);
        NXLOG_I("fs", "uncommitted %s of process %lu discarded (%s)", is_work ? "work folder" : "journal", static_cast<unsigned long>(pid), name.c_str());
    }
}

uint32_t Volume::create_file(const std::string& path, int64_t size) {
    if (read_only_) return rc::kReadOnlyFileSystem;
    if (size < 0) return rc::kOutOfRange;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (path == "/") return rc::kPathAlreadyExists;
    if (p_->lookup(path).exists) return rc::kPathAlreadyExists;
    if (const uint32_t r = p_->ensure_parents(path)) return r;
    HANDLE h = open_host(p_->wpath(path), true, CREATE_NEW);
    if (h == INVALID_HANDLE_VALUE) {
        NXLOG_E("fs", "create_file %s -> %s failed (win32 %lu)", path.c_str(), to_u8(p_->wpath(path)).c_str(),
                static_cast<unsigned long>(GetLastError()));
        return p_->host_fail(GetLastError());
    }
    uint32_t r = 0;
    if (size) {
        LARGE_INTEGER li;
        li.QuadPart = size;
        if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN) || !SetEndOfFile(h)) r = p_->host_fail(GetLastError());
    }
    CloseHandle(h);
    p_->tomb.erase(path);   // harmless when absent; a re-created path is served from the work layer
    return r;
}

uint32_t Volume::delete_file(const std::string& path) {
    if (read_only_) return rc::kReadOnlyFileSystem;
    std::lock_guard<std::mutex> lk(p_->mu);
    const Impl::Found f = p_->lookup(path);
    if (!f.exists || f.is_dir) return rc::kPathNotFound;
    std::error_code ec;
    stdfs::remove(p_->wpath(path), ec);
    if (ec) return p_->host_fail(ec);
    if (p_->committed_visible(path)) p_->tomb.insert(path);
    return 0;
}

uint32_t Volume::create_dir(const std::string& path) {
    if (read_only_) return rc::kReadOnlyFileSystem;
    std::lock_guard<std::mutex> lk(p_->mu);
    if (path == "/" || p_->lookup(path).exists) return rc::kPathAlreadyExists;
    if (const uint32_t r = p_->ensure_parents(path)) return r;
    std::error_code ec;
    stdfs::create_directory(p_->wpath(path), ec);
    return ec ? p_->host_fail(ec) : 0;
}

uint32_t Volume::delete_dir(const std::string& path) {
    if (read_only_) return rc::kReadOnlyFileSystem;
    std::lock_guard<std::mutex> lk(p_->mu);
    const Impl::Found f = p_->lookup(path);
    if (path == "/" || !f.exists || !f.is_dir) return rc::kPathNotFound;
    std::vector<DirItem> items;
    p_->list(path, &items);
    if (!items.empty()) return rc::kDirectoryNotEmpty;
    std::error_code ec;
    stdfs::remove(p_->wpath(path), ec);
    if (p_->committed_visible(path)) p_->tomb.insert(path);
    return 0;
}

uint32_t Volume::delete_dir_recursive(const std::string& path) {
    if (read_only_) return rc::kReadOnlyFileSystem;
    std::lock_guard<std::mutex> lk(p_->mu);
    const Impl::Found f = p_->lookup(path);
    if (path == "/" || !f.exists || !f.is_dir) return rc::kPathNotFound;
    std::error_code ec;
    stdfs::remove_all(p_->wpath(path), ec);
    if (ec) return p_->host_fail(ec);
    if (p_->committed_visible(path)) p_->tomb.insert(path);
    // work-layer tombstones below a re-created directory no longer apply
    for (auto it = p_->tomb.begin(); it != p_->tomb.end();)
        it = (it->size() > path.size() && it->compare(0, path.size(), path) == 0 && (*it)[path.size()] == '/') ? p_->tomb.erase(it) : std::next(it);
    return 0;
}

uint32_t Volume::clean_dir_recursive(const std::string& path) {
    if (read_only_) return rc::kReadOnlyFileSystem;
    std::vector<DirItem> items;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        if (const uint32_t r = p_->list(path, &items)) return r;
    }
    for (const DirItem& it : items) {
        const std::string child = (path == "/" ? "" : path) + "/" + it.name;
        const uint32_t r = it.is_dir ? delete_dir_recursive(child) : delete_file(child);
        if (r) return r;
    }
    return 0;
}

uint32_t Volume::rename_file(const std::string& from, const std::string& to) {
    if (read_only_) return rc::kReadOnlyFileSystem;
    std::lock_guard<std::mutex> lk(p_->mu);
    const Impl::Found a = p_->lookup(from);
    if (!a.exists || a.is_dir) return rc::kPathNotFound;
    if (from == to) return 0;
    if (p_->lookup(to).exists) return rc::kPathAlreadyExists;
    if (const uint32_t r = p_->ensure_parents(to)) return r;
    if (const uint32_t r = p_->materialize_file(from)) return r;
    std::error_code ec;
    stdfs::rename(p_->wpath(from), p_->wpath(to), ec);
    if (ec) return p_->host_fail(ec);
    if (p_->committed_visible(from)) p_->tomb.insert(from);
    return 0;
}

uint32_t Volume::rename_dir(const std::string& from, const std::string& to) {
    if (read_only_) return rc::kReadOnlyFileSystem;
    std::lock_guard<std::mutex> lk(p_->mu);
    const Impl::Found a = p_->lookup(from);
    if (from == "/" || !a.exists || !a.is_dir) return rc::kPathNotFound;
    if (from == to) return 0;
    if (to.size() > from.size() && to.compare(0, from.size(), from) == 0 && to[from.size()] == '/') return rc::kInvalidArgument;   // into itself
    if (p_->lookup(to).exists) return rc::kPathAlreadyExists;
    if (const uint32_t r = p_->ensure_parents(to)) return r;
    if (const uint32_t r = p_->materialize_tree(from)) return r;
    std::error_code ec;
    stdfs::rename(p_->wpath(from), p_->wpath(to), ec);
    if (ec) return p_->host_fail(ec);
    if (p_->committed_visible(from)) p_->tomb.insert(from);
    return 0;
}

uint32_t Volume::entry_type(const std::string& path, uint32_t* type) {
    std::lock_guard<std::mutex> lk(p_->mu);
    const Impl::Found f = p_->lookup(path);
    if (!f.exists) return rc::kPathNotFound;
    *type = f.is_dir ? 0 : 1;
    return 0;
}

uint32_t Volume::open_file(const std::string& path, uint32_t mode, std::shared_ptr<VFile>* out) {
    std::lock_guard<std::mutex> lk(p_->mu);
    const Impl::Found f = p_->lookup(path);
    if (!f.exists || f.is_dir) return rc::kPathNotFound;
    const bool write = (mode & (kModeWrite | kModeAppend)) != 0;
    if (write && read_only_) return rc::kReadOnlyFileSystem;
    stdfs::path host = f.host;
    if (write) {
        if (const uint32_t r = p_->materialize_file(path)) return r;
        host = p_->wpath(path);
    }
    HANDLE h = open_host(host, write, OPEN_EXISTING);
    if (h == INVALID_HANDLE_VALUE) return rc::kPathNotFound;
    *out = std::make_shared<HostFile>(h, mode, write && p_->journal ? p_->taint : nullptr);
    return 0;
}

uint32_t Volume::list_dir(const std::string& path, std::vector<DirItem>* out) {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->list(path, out);
}

uint32_t Volume::file_times(const std::string& path, FileTimes* out) {
    std::lock_guard<std::mutex> lk(p_->mu);
    const Impl::Found f = p_->lookup(path);
    if (!f.exists) return rc::kPathNotFound;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(f.host.c_str(), GetFileExInfoStandard, &d)) return rc::kGenericFailure;
    auto posix = [](const FILETIME& ft) {
        const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        return static_cast<int64_t>(t / 10000000ull) - 11644473600ll;
    };
    out->created = posix(d.ftCreationTime);
    out->modified = posix(d.ftLastWriteTime);
    out->accessed = posix(d.ftLastAccessTime);
    out->valid = true;
    return 0;
}

uint64_t Volume::used_bytes() {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->used("/");
}

namespace {

// Flushes one changed file before the commit is decided. 0 when it is on disk (or the file system does not flush), else the Win32 error.
// `opened` tells whether the file could be opened at all: a file held without sharing (an antivirus scan) gets a few short retries.
DWORD flush_path(const stdfs::path& p, bool* opened) {
    *opened = false;
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int tries = 0; tries < 6 && h == INVALID_HANDLE_VALUE; ++tries) {
        if (tries) Sleep(50 * tries);
        h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    *opened = true;
    DWORD e = 0;
    if (!injected_failure(Volume::IoPoint::kCommitFlush, &e) && !FlushFileBuffers(h)) e = GetLastError();
    CloseHandle(h);
    return flush_unsupported(e) ? 0 : e;
}

}  // namespace

uint32_t Volume::commit() {
    if (!p_->journal) return 0;
    if (read_only_) return 0;
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    RootLock lock(to_u8(s.root));
    std::error_code ec;
    if (s.tomb.empty() && !stdfs::exists(s.work, ec)) return 0;
    const std::string root_u8 = to_u8(s.root);

    // A commit that cannot go ahead. The committed folder is not touched either way. `drop` (the work folder holds a truncated or torn
    // file) also discards the uncommitted changes, so the game's next save starts again from the last good one; otherwise they stay and
    // the next Commit tries again. Either way the player is told (fsapi::save_failures) and the game gets a failure result.
    auto refuse = [&](DWORD e, const char* what, bool drop) -> uint32_t {
        std::error_code e2;
        stdfs::remove(s.journal_file, e2);
        bool left = false;
        if (drop) {
            left = !s.discard_work();
            if (left) s.taint->set(e);   // still tainted: the next commit is refused again and retries the discard
        }
        NXLOG_E("fs", "save commit of %s refused: %s (host error %lu); the last committed save is kept%s", root_u8.c_str(), what,
                static_cast<unsigned long>(e), !drop ? ", the changes stay for the next commit"
                                           : left ? "; [the work folder could not be emptied: files are still held open]"
                                                  : ", the uncommitted changes were dropped");
        report_save_failure(e);
        return host_rc(e);
    };
    if (const uint32_t e = s.taint->err.exchange(0)) return refuse(e, "a save write failed since the last commit", true);

    // Phase 1: flush the changed files, write the change list, make it the decided marker with one rename.
    std::string body = "W " + to_u8(s.work) + "\nR " + root_u8 + "\n";
    for (const std::string& t : s.tomb) body += "D " + rel_of(t) + "\n";
    int changes = static_cast<int>(s.tomb.size());
    std::vector<std::string> files, dirs;
    std::error_code walk;
    for (stdfs::recursive_directory_iterator it(s.work, walk), end; !walk && it != end; it.increment(walk)) {
        std::error_code e2;
        std::string g = to_u8(stdfs::relative(it->path(), s.work, e2));
        std::replace(g.begin(), g.end(), '\\', '/');
        const bool is_dir = !e2 && it->is_directory(e2);
        if (e2) return refuse(static_cast<DWORD>(e2.value()), "the changed files could not be listed", false);
        if (is_dir) {
            dirs.push_back(g);
            continue;
        }
        files.push_back(g);
        bool opened = false;
        if (const DWORD fe = flush_path(it->path(), &opened)) {
            // Could not open it: the data the game wrote is intact, only held (try again later). Opened but the flush failed: a full
            // disk or an I/O error lost cached data, the file is suspect.
            return refuse(fe, opened ? "flushing a changed file failed" : "a changed file is held by another program", opened);
        }
    }
    if (walk) return refuse(static_cast<DWORD>(walk.value()), "the changed files could not be listed", false);
    for (const std::string& d : dirs) body += "M " + d + "\n";
    for (const std::string& f : files) body += "F " + f + "\n";
    changes += static_cast<int>(dirs.size() + files.size());
    {
        std::ofstream j(s.journal_file, std::ios::binary | std::ios::trunc);
        j << body;
        j.flush();
        if (!j) return refuse(ERROR_DISK_FULL, "writing the commit journal failed", false);   // ofstream keeps no error code; a full disk is the likely one
    }
    {
        bool opened = false;
        if (const DWORD fe = flush_path(s.journal_file, &opened)) return refuse(fe, "flushing the commit journal failed", false);
    }
    DWORD me = 0;
    if (injected_failure(IoPoint::kDecide, &me) ||
        !MoveFileExW(s.journal_file.c_str(), s.marker.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (!me) me = GetLastError();
        return refuse(me, "the commit marker could not be written", false);
    }
    if (g_crash_point == 1) return rc::kGenericFailure;   // decided, nothing applied

    // Phase 2: apply. The commit is decided: a failure in here leaves the marker, which the next commit replaces with a superset (the
    // files still in the work folder plus the same deletions) and the next open of the save finishes (recover()).
    const int applied = apply_marker(s.marker, g_crash_point == 2 ? 1 : 0);
    if (g_crash_point == 2) return rc::kGenericFailure;
    if (applied < 0) {
        NXLOG_E("fs", "save commit of %s: applying it failed; it is finished by the next commit or the next start", root_u8.c_str());
        report_save_failure(ERROR_SHARING_VIOLATION);
        return rc::kGenericFailure;
    }
    s.tomb.clear();
    stdfs::remove_all(s.work, ec);
    stdfs::create_directories(s.work, ec);
    NXLOG_D("fs", "commit %s: %d changes", root_u8.c_str(), changes);
    return 0;
}

}  // namespace nxrt::services::fs

namespace nxrt::services::fsapi {

SaveFailures save_failures() {
    SaveFailures f;
    f.host_error = fs::g_save_fail_err.load();
    f.count = fs::g_save_fail_count.load();
    return f;
}

const char* save_failure_reason(uint32_t host_error) {
    switch (host_error) {
        case ERROR_DISK_FULL:
        case ERROR_HANDLE_DISK_FULL: return "the disk is full";
        case ERROR_SHARING_VIOLATION:
        case ERROR_LOCK_VIOLATION:
        case ERROR_ACCESS_DENIED: return "a save file is held by another program";
        default: return "a disk error";
    }
}

}  // namespace nxrt::services::fsapi
