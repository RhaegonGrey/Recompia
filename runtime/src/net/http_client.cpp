// SPDX-License-Identifier: GPL-3.0-or-later
// The host HTTP(S) client (http_client.h): WinHTTP on Windows, the system's libcurl on the native Linux and macOS builds, both in their
// synchronous mode on one worker thread per request. libcurl is loaded by name at the first request (nothing is linked against it), so a
// host without it loses HTTPS for mods rather than failing to start the game; see the POSIX block at the bottom.
#include "http_client.h"

#if defined(_WIN32)
#include <windows.h>
#include <winhttp.h>
#endif

#include "nxrt/log.h"
#if defined(_WIN32)
#include <atomic>
#include <thread>

#include "nxrt/text.h"
#else
#include <dlfcn.h>

#include <atomic>
#include <cctype>
#include <mutex>
#include <thread>
#endif

#if defined(_WIN32)
#pragma comment(lib, "winhttp.lib")
#endif

namespace nxrt::http {

#if defined(_WIN32)

namespace {

constexpr const char* kTag = "http";

std::atomic<int> g_in_flight{0};

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET x) : h(x) {}
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

// The parts of an allowed URL, or false: https, or http to localhost / 127.0.0.1 only; no user name or password in it.
struct Target {
    std::wstring host, path;
    INTERNET_PORT port = 0;
    bool tls = true;
};
bool parse(const std::string& url, Target* t) {
    const std::wstring w = text::widen(url);
    URL_COMPONENTS uc = {sizeof uc};
    uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    uc.dwUserNameLength = uc.dwPasswordLength = static_cast<DWORD>(-1);
    if (w.empty() || !WinHttpCrackUrl(w.c_str(), static_cast<DWORD>(w.size()), 0, &uc)) return false;
    if (uc.dwUserNameLength || uc.dwPasswordLength || !uc.dwHostNameLength) return false;
    t->host.assign(uc.lpszHostName, uc.dwHostNameLength);
    t->path.assign(uc.lpszUrlPath ? uc.lpszUrlPath : L"", uc.dwUrlPathLength);
    if (uc.lpszExtraInfo) t->path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (t->path.empty()) t->path = L"/";
    t->port = uc.nPort;
    if (uc.nScheme == INTERNET_SCHEME_HTTPS) return t->tls = true;
    t->tls = false;
    return uc.nScheme == INTERNET_SCHEME_HTTP && (_wcsicmp(t->host.c_str(), L"localhost") == 0 || t->host == L"127.0.0.1");
}

// One session for the process (WinHTTP handles are safe across threads): no proxy sign-in, no redirects, TLS 1.2 or later.
HINTERNET session(const std::string& user_agent) {
    static HINTERNET s = [&] {
        const std::wstring ua = text::widen(user_agent.empty() ? std::string("nxrt") : user_agent);
        HINTERNET h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!h) h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);   // before 8.1
        if (!h) {
            NXLOG_W(kTag, "WinHttpOpen failed (%lu): no host HTTP requests this run", GetLastError());
            return h;
        }
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(h, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof redirect);
        DWORD tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
        if (!WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls)) {   // a WinHTTP without TLS 1.3 refuses the flag
            tls = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
            WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &tls, sizeof tls);
        }
        return h;
    }();
    return s;
}

// The request itself, on the worker: the status and the body, or 0 when there is no answer.
int run(const Request& r, const Target& t, std::string* out) {
    HINTERNET s = session(r.user_agent);
    if (!s) return 0;
    Handle con(WinHttpConnect(s, t.host.c_str(), t.port, 0));
    if (!con.h) return 0;
    const std::wstring verb = text::widen(r.method);
    Handle req(WinHttpOpenRequest(con.h, verb.c_str(), t.path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  t.tls ? WINHTTP_FLAG_SECURE : 0));
    if (!req.h) return 0;
    const int to = r.step_timeout_ms;
    WinHttpSetTimeouts(req.h, to, to, to, to);
    DWORD off = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_AUTHENTICATION;
    if (!WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &off, sizeof off)) return 0;   // never send without these off
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;   // never the Windows account's credentials
    WinHttpSetOption(req.h, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof logon);
    std::wstring headers;
    if (!r.body.empty()) headers = L"Content-Type: " + text::widen(r.content_type);
    void* body = r.body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(r.body.data());
    const DWORD n = static_cast<DWORD>(r.body.size());
    if (!WinHttpSendRequest(req.h, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(), headers.empty() ? 0 : static_cast<DWORD>(-1),
                            body, n, n, 0) ||
        !WinHttpReceiveResponse(req.h, nullptr))
        return 0;
    DWORD status = 0, size = sizeof status;
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                             WINHTTP_NO_HEADER_INDEX))
        return 0;
    out->clear();
    for (;;) {
        char buf[16384];
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf, sizeof buf, &got)) return 0;
        if (got == 0) break;
        if (out->size() + got > r.max_answer) return 0;   // too long an answer counts as none: stop reading
        out->append(buf, got);
    }
    return static_cast<int>(status);
}

}  // namespace

bool start(Request r, DoneFn done) {
    if (!done || (r.method != "GET" && r.method != "POST")) return false;
    Target t;
    if (!parse(r.url, &t)) return false;
    if (g_in_flight.fetch_add(1) >= kMaxInFlight) {
        g_in_flight.fetch_sub(1);
        return false;
    }
    try {
        std::thread([r = std::move(r), t = std::move(t), done = std::move(done)] {
            std::string body;
            const int status = run(r, t, &body);
            if (!status) body.clear();
            g_in_flight.fetch_sub(1);
            done(status, std::move(body));
        }).detach();
    } catch (...) {   // no thread could be made
        g_in_flight.fetch_sub(1);
        return false;
    }
    return true;
}

#else   // native Linux and macOS: the system's libcurl, found at run time

// WinHTTP is Windows-only, and these builds link no TLS stack of their own. libcurl is on every Linux desktop (SteamOS, Debian, Fedora,
// Arch all ship it, Steam itself links it) and in macOS's dyld shared cache (/usr/lib/libcurl.4.dylib), so it is loaded by name at the
// first request: nothing is linked against it, so a host without it loses HTTPS for mods instead of the game failing to start. The easy
// interface's ABI has been stable since libcurl 7.0, so the handful of calls a request makes are declared here rather than pulling in
// curl's headers, which would make a build-time dependency of a run-time one.
//
// One easy handle per request, on its own worker, blocking: the same shape as the WinHTTP path above, and the same rules (http_client.h).

namespace {

constexpr const char* kTag = "http";

using Handle = void;   // CURL*: the easy handle

struct Slist {   // struct curl_slist, which is part of the ABI
    char* data;
    Slist* next;
};

// curl.h's CURLoption / CURLINFO values, unchanged since 7.0: a type tag plus the number.
enum : int {
    CURLOPT_WRITEDATA = 10001,
    CURLOPT_URL = 10002,
    CURLOPT_POSTFIELDS = 10015,
    CURLOPT_USERAGENT = 10018,
    CURLOPT_HTTPHEADER = 10023,
    CURLOPT_CUSTOMREQUEST = 10036,
    CURLOPT_FOLLOWLOCATION = 52,
    CURLOPT_NOSIGNAL = 99,
    CURLOPT_SSLVERSION = 32,
    CURLOPT_CONNECTTIMEOUT_MS = 156,
    CURLOPT_LOW_SPEED_LIMIT = 19,
    CURLOPT_LOW_SPEED_TIME = 20,
    CURLOPT_POSTFIELDSIZE_LARGE = 30120,
    CURLOPT_WRITEFUNCTION = 20011,
    CURLINFO_RESPONSE_CODE = 0x200000 + 2,
};
constexpr long kTls12OrLater = 6;   // CURL_SSLVERSION_TLSv1_2, which means 1.2 or later
constexpr long kGlobalAll = 3;      // CURL_GLOBAL_ALL
constexpr int kWriteError = 23;     // CURLE_WRITE_ERROR: the write callback said stop (an answer past max_answer)

// The calls a request makes. easy_setopt and easy_getinfo are variadic in curl.h too, and the arguments are read by type exactly as
// there; their return values are ignored because every option below exists in any libcurl this can meet (7.16.2 for the timeouts).
struct Lib {
    bool ok = false;
    void* so = nullptr;
    const char* version = nullptr;
    int (*global_init)(long) = nullptr;
    Handle* (*easy_init)() = nullptr;
    int (*easy_setopt)(Handle*, int, ...) = nullptr;
    int (*easy_perform)(Handle*) = nullptr;
    void (*easy_cleanup)(Handle*) = nullptr;
    int (*easy_getinfo)(Handle*, int, ...) = nullptr;
    const char* (*easy_strerror)(int) = nullptr;
    Slist* (*slist_append)(Slist*, const char*) = nullptr;
    void (*slist_free_all)(Slist*) = nullptr;
};

// dlopen by soname: libcurl.so.4 and libcurl.4.dylib are what the distributions and macOS ship. The unversioned names are for a
// development install that has no soname symlink.
Lib load() {
#if defined(__APPLE__)
    static const char* const kNames[] = {"/usr/lib/libcurl.4.dylib", "libcurl.4.dylib", "libcurl.dylib"};
#else
    static const char* const kNames[] = {"libcurl.so.4", "libcurl.so"};
#endif
    Lib l;
    const char* opened = nullptr;
    for (const char* name : kNames) {
        l.so = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (l.so) {
            opened = name;
            break;
        }
    }
    if (!l.so) {
        NXLOG_W(kTag, "no libcurl (%s): a mod's HTTPS request has no answer on this host", ::dlerror());
        return l;
    }
    const auto sym = [&l](const char* name) { return ::dlsym(l.so, name); };
    l.global_init = reinterpret_cast<decltype(l.global_init)>(sym("curl_global_init"));
    l.easy_init = reinterpret_cast<decltype(l.easy_init)>(sym("curl_easy_init"));
    l.easy_setopt = reinterpret_cast<decltype(l.easy_setopt)>(sym("curl_easy_setopt"));
    l.easy_perform = reinterpret_cast<decltype(l.easy_perform)>(sym("curl_easy_perform"));
    l.easy_cleanup = reinterpret_cast<decltype(l.easy_cleanup)>(sym("curl_easy_cleanup"));
    l.easy_getinfo = reinterpret_cast<decltype(l.easy_getinfo)>(sym("curl_easy_getinfo"));
    l.easy_strerror = reinterpret_cast<decltype(l.easy_strerror)>(sym("curl_easy_strerror"));
    l.slist_append = reinterpret_cast<decltype(l.slist_append)>(sym("curl_slist_append"));
    l.slist_free_all = reinterpret_cast<decltype(l.slist_free_all)>(sym("curl_slist_free_all"));
    const auto version = reinterpret_cast<const char* (*)()>(sym("curl_version"));
    if (version) l.version = version();
    l.ok = l.global_init && l.easy_init && l.easy_setopt && l.easy_perform && l.easy_cleanup && l.easy_getinfo && l.easy_strerror &&
           l.slist_append && l.slist_free_all;
    if (!l.ok) {
        ::dlclose(l.so);
        l.so = nullptr;
        NXLOG_W(kTag, "libcurl at %s has no call this client needs: a mod's HTTPS request has no answer", opened);
        return l;
    }
    NXLOG_I(kTag, "libcurl %s (%s): HTTPS requests go through it", l.version ? l.version : "of unknown version", opened);
    return l;
}

Lib& lib() {
    static Lib l = load();
    return l;
}

std::atomic<int> g_in_flight{0};

// The rules of the WinHTTP path, applied to the URL as written: https, or http to localhost / 127.0.0.1 only; no user name or password
// in it (nothing to sign in with); no control character, which could split a request line or a header.
bool allowed(const std::string& url, bool* tls) {
    const size_t sep = url.find("://");
    if (sep == std::string::npos) return false;
    std::string scheme = url.substr(0, sep);
    for (char& c : scheme) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (scheme != "https" && scheme != "http") return false;
    for (const char c : url) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) return false;
    }
    // The authority runs to the next '/', '?' or '#'. libcurl takes the URL as given, so this is the whole check: the host it will
    // reach is the host read here.
    size_t end = url.find_first_of("/?#", sep + 3);
    if (end == std::string::npos) end = url.size();
    std::string host = url.substr(sep + 3, end - sep - 3);
    if (host.empty() || host.find('@') != std::string::npos) return false;
    const size_t close = host.rfind(']');   // an IPv6 literal keeps its brackets and its colons
    const size_t colon = host.rfind(':');
    if (colon != std::string::npos && (close == std::string::npos || colon > close)) {
        const std::string port = host.substr(colon + 1);
        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return false;
        if (std::stoi(port) < 1 || std::stoi(port) > 65535) return false;
        host.resize(colon);
    }
    if (host.empty()) return false;
    *tls = scheme == "https";
    if (*tls) return true;
    std::string bare = host;
    for (char& c : bare) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return bare == "localhost" || host == "127.0.0.1";
}

struct Body {
    std::string* out;
    size_t max;
};

// Returning less than it was given aborts the transfer, which is how an answer past max_answer becomes no answer at all.
size_t write_body(char* p, size_t size, size_t nmemb, void* ud) {
    auto* b = static_cast<Body*>(ud);
    const size_t n = size * nmemb;
    if (b->out->size() + n > b->max) return 0;
    b->out->append(p, n);
    return n;
}

// One request: the status and the body, or 0 when there is no answer.
int run(const Request& r, bool tls, std::string* out) {
    Lib& l = lib();
    if (!l.ok) return 0;
    static std::once_flag once;
    std::call_once(once, [&l] { l.global_init(kGlobalAll); });   // once for the process, before any handle exists
    Handle* h = l.easy_init();
    if (!h) return 0;
    struct Close {
        Lib& l;
        Handle* h;
        ~Close() { l.easy_cleanup(h); }
    } close{l, h};
    Body body{out, r.max_answer};
    const long step = r.step_timeout_ms > 0 ? r.step_timeout_ms : 10000;
    l.easy_setopt(h, CURLOPT_URL, r.url.c_str());
    l.easy_setopt(h, CURLOPT_NOSIGNAL, 1L);   // no SIGALRM: this runs on a worker beside the game
    l.easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);   // a 3xx is the answer, not a step
    l.easy_setopt(h, CURLOPT_USERAGENT, r.user_agent.empty() ? "nxrt" : r.user_agent.c_str());
    l.easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, step);   // name lookup and connect
    l.easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);       // a transfer that moves no byte for `step` seconds has no answer
    l.easy_setopt(h, CURLOPT_LOW_SPEED_TIME, step < 1000 ? 1L : step / 1000);
    if (tls) l.easy_setopt(h, CURLOPT_SSLVERSION, kTls12OrLater);
    l.easy_setopt(h, CURLOPT_WRITEFUNCTION, &write_body);
    l.easy_setopt(h, CURLOPT_WRITEDATA, &body);
    Slist* headers = nullptr;
    l.easy_setopt(h, CURLOPT_CUSTOMREQUEST, r.method.c_str());   // the verb as asked: a GET with a body stays a GET
    if (r.method == "POST" || !r.body.empty()) {
        // A body needs its type and its length (a std::string may hold a 0 byte). A POST without one still sends Content-Length: 0 and
        // no Content-Type, which is what the WinHTTP path sends: the empty header below removes curl's form default.
        const std::string type = r.body.empty() ? std::string("Content-Type:") : "Content-Type: " + r.content_type;
        headers = l.slist_append(nullptr, type.c_str());
        if (headers) l.easy_setopt(h, CURLOPT_HTTPHEADER, headers);
        l.easy_setopt(h, CURLOPT_POSTFIELDS, r.body.empty() ? "" : r.body.data());
        l.easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<long long>(r.body.size()));
    }
    const int rc = l.easy_perform(h);
    if (headers) l.slist_free_all(headers);
    long status = 0;
    l.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    if (rc != 0) {
        if (rc != kWriteError) NXLOG_D(kTag, "%s: %s", r.url.c_str(), l.easy_strerror(rc));
        return 0;
    }
    return static_cast<int>(status);
}

}  // namespace

bool start(Request r, DoneFn done) {
    if (!done || (r.method != "GET" && r.method != "POST")) return false;
    bool tls = true;
    if (!allowed(r.url, &tls)) return false;
    if (!lib().ok) return false;   // no libcurl on this host: refused up front, not a thread that answers "no answer"
    if (g_in_flight.fetch_add(1) >= kMaxInFlight) {
        g_in_flight.fetch_sub(1);
        return false;
    }
    try {
        std::thread([r = std::move(r), tls, done = std::move(done)] {
            std::string body;
            const int status = run(r, tls, &body);
            if (!status) body.clear();
            g_in_flight.fetch_sub(1);
            done(status, std::move(body));
        }).detach();
    } catch (...) {   // no thread could be made
        g_in_flight.fetch_sub(1);
        return false;
    }
    return true;
}

#endif

}  // namespace nxrt::http
