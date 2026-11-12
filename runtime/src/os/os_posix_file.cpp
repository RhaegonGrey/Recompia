// SPDX-License-Identifier: GPL-3.0-or-later
// os_posix_file.cpp: the POSIX side of src/os/nx_os.h -- CreateFileW/ReadFile/WriteFile and the directory walks.
//
// Two Win32 behaviours are load-bearing here and are reproduced exactly:
//   - ReadFile/WriteFile take the position from an OVERLAPPED offset, not from the file pointer, so several threads
//     may read one handle at once with no seek lock (fs_romfs.cpp:552 says so). That is pread/pwrite.
//   - A short read at the end of the file is not an error; a read that starts at or past EOF returns FALSE with
//     ERROR_HANDLE_EOF (fs_volume.cpp:101 branches on exactly that).
//
// Windows path separators are accepted: the callers build paths with L"\\" in places (nvn_shaders.cpp:1132) while
// std::filesystem::path on POSIX uses '/'. A backslash is a legal file name character on macOS, so it is rewritten
// only as a separator, which is what it always is at these call sites.
#include "nx_os_internal.h"

#if !defined(_WIN32)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace nxos;

namespace {

FileObj* as_file(HANDLE h) { return as<FileObj>(h, Kind::File); }
FindObj* as_find(HANDLE h) { return as<FindObj>(h, Kind::Find); }

// Splits a find pattern into the directory to list and the leaf pattern to match.
void split_pattern(const std::string& path, std::string* dir, std::string* leaf) {
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        *dir = ".";
        *leaf = path;
    } else {
        *dir = slash == 0 ? "/" : path.substr(0, slash);
        *leaf = path.substr(slash + 1);
    }
    if (dir->empty()) *dir = ".";
}

void set_time(FILETIME* ft, const struct timespec& ts) {
    const uint64_t v = 11644473600ull * 10000000ull + static_cast<uint64_t>(ts.tv_sec) * 10000000ull +
                       static_cast<uint64_t>(ts.tv_nsec) / 100ull;
    ft->dwLowDateTime = static_cast<DWORD>(v & 0xFFFFFFFFu);
    ft->dwHighDateTime = static_cast<DWORD>(v >> 32);
}

void fill_find_data(const std::string& name, const struct stat& st, WIN32_FIND_DATAW* data) {
    std::memset(data, 0, sizeof *data);
    data->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    data->nFileSizeHigh = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) >> 32);
    data->nFileSizeLow = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) & 0xFFFFFFFFu);
#if defined(__APPLE__)
    set_time(&data->ftCreationTime, st.st_birthtimespec);
    set_time(&data->ftLastWriteTime, st.st_mtimespec);
#else
    set_time(&data->ftCreationTime, st.st_ctim);
    set_time(&data->ftLastWriteTime, st.st_mtim);
#endif
    data->ftLastAccessTime = data->ftLastWriteTime;
    // cFileName is a WCHAR array: the name is UTF-8 here, so convert it. A name longer than MAX_PATH-1 is truncated,
    // which no caller in this tree produces (they all list romfs files with short names).
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), static_cast<int>(name.size()), data->cFileName, MAX_PATH - 1);
}

}  // namespace

// ==================================================================================================================
// files
// ==================================================================================================================
HANDLE nxos_create_file(const char* path, DWORD access, DWORD share, void*, DWORD disposition, DWORD flags) {
    if (!path || !*path) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    (void)share;   // POSIX has no mandatory share modes; every caller asks for "deny nothing"
    (void)flags;
    const std::string p = native_path(path);
    const bool want_write = (access & (GENERIC_WRITE | GENERIC_ALL)) != 0;

    int oflags;
    switch (disposition) {
        case CREATE_NEW: oflags = O_CREAT | O_EXCL; break;
        case CREATE_ALWAYS: oflags = O_CREAT | O_TRUNC; break;
        case OPEN_EXISTING: oflags = 0; break;
        case OPEN_ALWAYS: oflags = O_CREAT; break;
        case TRUNCATE_EXISTING: oflags = O_TRUNC; break;
        default:
            SetLastError(ERROR_INVALID_PARAMETER);
            return INVALID_HANDLE_VALUE;
    }
    oflags |= want_write ? O_RDWR : O_RDONLY;

    int fd = ::open(p.c_str(), oflags, 0644);
    bool is_dir = false;
    if (fd < 0 && (errno == EISDIR || errno == EACCES)) {
        // A directory: opened read-only so FlushFileBuffers can fsync it (fs_volume.cpp flushes directories).
        fd = ::open(p.c_str(), O_RDONLY);
        is_dir = fd >= 0;
    }
    if (fd < 0) {
        nxos_set_error_from_errno(errno);
        return INVALID_HANDLE_VALUE;
    }
    if (!is_dir) {
        struct stat st{};
        if (fstat(fd, &st) == 0) is_dir = S_ISDIR(st.st_mode);
    }
    if (want_write && is_dir) {
        ::close(fd);
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return new FileObj(fd, is_dir);
}

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD want, LPDWORD got, LPOVERLAPPED ov) {
    if (got) *got = 0;
    FileObj* f = as_file(h);
    if (!f || !buf) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (f->is_dir) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    const bool positioned = ov != nullptr;
    const uint64_t off = positioned ? (static_cast<uint64_t>(ov->OffsetHigh) << 32) | ov->Offset : 0;

    size_t done = 0;
    while (done < want) {
        const ssize_t n = positioned ? ::pread(f->fd, static_cast<char*>(buf) + done, want - done,
                                              static_cast<off_t>(off + done))
                                     : ::read(f->fd, static_cast<char*>(buf) + done, want - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            nxos_set_error_from_errno(errno);
            return FALSE;
        }
        if (n == 0) break;   // EOF
        done += static_cast<size_t>(n);
    }
    if (got) *got = static_cast<DWORD>(done);
    if (done == 0 && want > 0) {
        // Windows: a read that starts at or past EOF fails with ERROR_HANDLE_EOF; a short read does not.
        SetLastError(ERROR_HANDLE_EOF);
        return FALSE;
    }
    return TRUE;
}

BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD want, LPDWORD wrote, LPOVERLAPPED ov) {
    if (wrote) *wrote = 0;
    FileObj* f = as_file(h);
    if (!f || !buf) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (f->is_dir) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    const bool positioned = ov != nullptr;
    const uint64_t off = positioned ? (static_cast<uint64_t>(ov->OffsetHigh) << 32) | ov->Offset : 0;

    size_t done = 0;
    while (done < want) {
        const ssize_t n = positioned ? ::pwrite(f->fd, static_cast<const char*>(buf) + done, want - done,
                                               static_cast<off_t>(off + done))
                                     : ::write(f->fd, static_cast<const char*>(buf) + done, want - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            nxos_set_error_from_errno(errno);
            return FALSE;
        }
        if (n == 0) break;
        done += static_cast<size_t>(n);
    }
    if (wrote) *wrote = static_cast<DWORD>(done);
    if (done != want) {
        nxos_set_error_from_errno(ENOSPC);
        return FALSE;
    }
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE h) {
    FileObj* f = as_file(h);
    if (!f) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    // fsync works on a directory fd on Darwin too, which is how a rename is made durable (fs_volume.cpp:697).
    if (::fsync(f->fd) != 0) {
        nxos_set_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL GetFileSizeEx(HANDLE h, PLARGE_INTEGER size) {
    FileObj* f = as_file(h);
    if (!f || !size) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    struct stat st{};
    if (fstat(f->fd, &st) != 0) {
        nxos_set_error_from_errno(errno);
        return FALSE;
    }
    size->QuadPart = static_cast<LONGLONG>(st.st_size);
    return TRUE;
}

DWORD GetFileSize(HANDLE h, LPDWORD high) {
    LARGE_INTEGER v{};
    if (!GetFileSizeEx(h, &v)) return INVALID_FILE_ATTRIBUTES;
    if (high) *high = static_cast<DWORD>(v.QuadPart >> 32);
    return static_cast<DWORD>(v.QuadPart & 0xFFFFFFFFu);
}

BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER dist, PLARGE_INTEGER new_pos, DWORD method) {
    FileObj* f = as_file(h);
    if (!f) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    const int whence = method == FILE_BEGIN ? SEEK_SET : (method == FILE_CURRENT ? SEEK_CUR : SEEK_END);
    const off_t pos = ::lseek(f->fd, static_cast<off_t>(dist.QuadPart), whence);
    if (pos < 0) {
        nxos_set_error_from_errno(errno);
        return FALSE;
    }
    if (new_pos) new_pos->QuadPart = pos;
    return TRUE;
}

BOOL SetEndOfFile(HANDLE h) {
    FileObj* f = as_file(h);
    if (!f) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    const off_t pos = ::lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0 || ::ftruncate(f->fd, pos) != 0) {
        nxos_set_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL ReadProcessMemory(HANDLE, LPCVOID addr, LPVOID out, SIZE_T size, PSIZE_T got) {
    // The guest address space is the host address space (identity map), so this is a copy. The callers use it to probe
    // an address without faulting (diag/sampler.cpp:76), so an unmapped range must fail rather than crash.
    if (got) *got = 0;
    if (!addr || !out) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    uintptr_t page = reinterpret_cast<uintptr_t>(addr);
    const uintptr_t end = page + size;
    while (page < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(page), &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= page) {
            SetLastError(ERROR_ACCESS_DENIED);
            return FALSE;
        }
        page = next;
    }
    std::memcpy(out, addr, size);
    if (got) *got = size;
    return TRUE;
}

BOOL WriteProcessMemory(HANDLE, LPVOID addr, LPCVOID src, SIZE_T size, PSIZE_T wrote) {
    if (wrote) *wrote = 0;
    if (!addr || !src) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    std::memcpy(addr, src, size);
    if (wrote) *wrote = size;
    return TRUE;
}

BOOL nxos_delete_file(const char* path) {
    if (!path) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (::unlink(native_path(path).c_str()) == 0) return TRUE;
    nxos_set_error_from_errno(errno);
    return FALSE;
}

BOOL nxos_move_file(const char* from, const char* to, DWORD flags) {
    if (!from || !to) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const std::string a = native_path(from), b = native_path(to);
    if (!(flags & MOVEFILE_REPLACE_EXISTING)) {
        struct stat st{};
        if (::stat(b.c_str(), &st) == 0) {
            SetLastError(ERROR_ALREADY_EXISTS);
            return FALSE;
        }
    }
    if (::rename(a.c_str(), b.c_str()) == 0) return TRUE;
    const int err = errno;
    if (flags & MOVEFILE_COPY_ALLOWED) {
        if (nxos_copy_file(from, to, FALSE)) {
            ::unlink(a.c_str());
            return TRUE;
        }
        return FALSE;
    }
    nxos_set_error_from_errno(err);
    return FALSE;
}

BOOL nxos_copy_file(const char* from, const char* to, BOOL fail_if_exists) {
    if (!from || !to) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const std::string a = native_path(from), b = native_path(to);
    if (fail_if_exists) {
        struct stat st{};
        if (::stat(b.c_str(), &st) == 0) {
            SetLastError(ERROR_ALREADY_EXISTS);
            return FALSE;
        }
    }
    const int in = ::open(a.c_str(), O_RDONLY);
    if (in < 0) {
        nxos_set_error_from_errno(errno);
        return FALSE;
    }
    const int out = ::open(b.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        nxos_set_error_from_errno(errno);
        ::close(in);
        return FALSE;
    }
    std::vector<char> buf(1u << 20);
    BOOL ok = TRUE;
    for (;;) {
        const ssize_t n = ::read(in, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            nxos_set_error_from_errno(errno);
            ok = FALSE;
            break;
        }
        if (n == 0) break;
        ssize_t done = 0;
        while (done < n) {
            const ssize_t w = ::write(out, buf.data() + done, static_cast<size_t>(n - done));
            if (w < 0) {
                if (errno == EINTR) continue;
                nxos_set_error_from_errno(errno);
                ok = FALSE;
                break;
            }
            done += w;
        }
        if (!ok) break;
    }
    ::close(in);
    ::close(out);
    return ok;
}

DWORD nxos_get_file_attributes(const char* path) {
    if (!path) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_FILE_ATTRIBUTES;
    }
    struct stat st{};
    if (::stat(native_path(path).c_str(), &st) != 0) {
        nxos_set_error_from_errno(errno);
        return INVALID_FILE_ATTRIBUTES;
    }
    DWORD a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (!(st.st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    return a;
}

BOOL nxos_set_file_attributes(const char* path, DWORD attrs) {
    if (!path) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const std::string p = native_path(path);
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) {
        nxos_set_error_from_errno(errno);
        return FALSE;
    }
    mode_t mode = st.st_mode;
    if (attrs & FILE_ATTRIBUTE_READONLY) mode &= static_cast<mode_t>(~(S_IWUSR | S_IWGRP | S_IWOTH));
    else mode |= S_IWUSR;
    if (::chmod(p.c_str(), mode) != 0) {
        nxos_set_error_from_errno(errno);
        return FALSE;
    }
    return TRUE;
}

BOOL GetFileAttributesExW(LPCWSTR path, int level, WIN32_FILE_ATTRIBUTE_DATA* data) {
    return nxos_get_file_attributes_ex(nxos_utf8(path).c_str(), data);
}

BOOL nxos_get_file_attributes_ex(const char* path, WIN32_FILE_ATTRIBUTE_DATA* data) {
    if (!path || !data) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    const std::string p = native_path(path);
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) {
        nxos_set_error_from_errno(errno);
        return FALSE;
    }
    std::memset(data, 0, sizeof *data);
    data->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    if (!(st.st_mode & S_IWUSR)) data->dwFileAttributes |= FILE_ATTRIBUTE_READONLY;
    data->nFileSizeHigh = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) >> 32);
    data->nFileSizeLow = static_cast<DWORD>(static_cast<uint64_t>(st.st_size) & 0xFFFFFFFFu);
    auto put = [](FILETIME* ft, const struct timespec& ts) {
        const uint64_t v = 11644473600ull * 10000000ull + static_cast<uint64_t>(ts.tv_sec) * 10000000ull +
                           static_cast<uint64_t>(ts.tv_nsec) / 100ull;
        ft->dwLowDateTime = static_cast<DWORD>(v & 0xFFFFFFFFu);
        ft->dwHighDateTime = static_cast<DWORD>(v >> 32);
    };
#if defined(__APPLE__)
    put(&data->ftCreationTime, st.st_birthtimespec);
    put(&data->ftLastWriteTime, st.st_mtimespec);
#else
    put(&data->ftCreationTime, st.st_ctim);
    put(&data->ftLastWriteTime, st.st_mtim);
#endif
    data->ftLastAccessTime = data->ftLastWriteTime;
    return TRUE;
}

HANDLE nxos_create_directory(const char* path) {
    if (!path) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    const std::string p = native_path(path);
    if (::mkdir(p.c_str(), 0755) == 0) return new FileObj(-1, true);
    if (errno == EEXIST) {
        SetLastError(ERROR_ALREADY_EXISTS);
        return INVALID_HANDLE_VALUE;
    }
    nxos_set_error_from_errno(errno);
    return INVALID_HANDLE_VALUE;
}

DWORD nxos_private_profile_string(const char* section, const char* key, const char* def, char* out, DWORD cap,
                                  const char* file) {
    // A plain ini read: the same rules services/common.cpp relies on (case-insensitive section and key names, the first
    // match wins, the value is trimmed of surrounding blanks). GetPrivateProfileStringA semantics, nothing more.
    if (!out || cap == 0) return 0;
    const std::string fallback = def ? def : "";
    std::string value = fallback;
    if (section && key && file) {
        if (FILE* f = std::fopen(file, "rb")) {
            char line[1024];
            bool in_section = false;
            while (std::fgets(line, sizeof line, f)) {
                std::string text(line);
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
                const size_t b = text.find_first_not_of(" \t");
                if (b == std::string::npos) continue;
                if (text[b] == ';' || text[b] == '#') continue;
                if (text[b] == '[') {
                    const size_t e = text.find(']', b);
                    if (e == std::string::npos) continue;
                    in_section = lstrcmpiA(text.substr(b + 1, e - b - 1).c_str(), section) == 0;
                    continue;
                }
                if (!in_section) continue;
                const size_t eq = text.find('=', b);
                if (eq == std::string::npos) continue;
                if (lstrcmpiA(text.substr(b, eq - b).c_str(), key) != 0) continue;
                const size_t vb = text.find_first_not_of(" \t", eq + 1);
                value = vb == std::string::npos ? std::string() : text.substr(vb);
                break;
            }
            std::fclose(f);
        }
    }
    const size_t n = std::min<size_t>(value.size(), cap - 1);
    std::memcpy(out, value.data(), n);
    out[n] = 0;
    return static_cast<DWORD>(n);
}

DWORD nxos_full_path(const char* path, char* out, DWORD cap) {
    if (!path) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    char buf[4096];
    if (!::realpath(native_path(path).c_str(), buf)) {
        nxos_set_error_from_errno(errno);
        return 0;
    }
    const size_t n = std::strlen(buf);
    if (!out || cap < n + 1) return static_cast<DWORD>(n + 1);
    std::memcpy(out, buf, n + 1);
    return static_cast<DWORD>(n);
}

BOOL nxos_remove_directory(const char* path) {
    if (!path) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (::rmdir(native_path(path).c_str()) == 0) return TRUE;
    nxos_set_error_from_errno(errno);
    return FALSE;
}

// ==================================================================================================================
// directory walks
// ==================================================================================================================
HANDLE nxos_find_first(const char* pattern, WIN32_FIND_DATAW* data) {
    if (!pattern || !data) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return INVALID_HANDLE_VALUE;
    }
    std::string dir, leaf;
    split_pattern(native_path(pattern), &dir, &leaf);

    DIR* d = ::opendir(dir.c_str());
    if (!d) {
        nxos_set_error_from_errno(errno);
        return INVALID_HANDLE_VALUE;
    }
    auto* find = new FindObj();
    find->dir = dir;
    while (const dirent* e = ::readdir(d)) {
        const char* name = e->d_name;
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
        if (::fnmatch(leaf.c_str(), name, 0) != 0) continue;
        find->names.emplace_back(name);
    }
    ::closedir(d);

    if (find->names.empty()) {
        delete find;
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    std::sort(find->names.begin(), find->names.end());
    find->at = 0;
    struct stat st{};
    if (::stat((dir + "/" + find->names[0]).c_str(), &st) != 0) std::memset(&st, 0, sizeof st);
    fill_find_data(find->names[0], st, data);
    return find;
}

BOOL nxos_find_next(HANDLE h, WIN32_FIND_DATAW* data) {
    FindObj* find = as_find(h);
    if (!find || !data) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    if (++find->at >= find->names.size()) {
        SetLastError(ERROR_NO_MORE_FILES);
        return FALSE;
    }
    struct stat st{};
    if (::stat((find->dir + "/" + find->names[find->at]).c_str(), &st) != 0) std::memset(&st, 0, sizeof st);
    fill_find_data(find->names[find->at], st, data);
    return TRUE;
}

#endif  // !_WIN32
