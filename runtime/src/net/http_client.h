// SPDX-License-Identifier: GPL-3.0-or-later
// A small host HTTP(S) client: one request at a time per call, each on its own worker thread, answered once through a callback. Used by the
// mods host (Modtopia's net_request); nothing here knows a game or a mod. WinHTTP on Windows, the system's libcurl on Linux and macOS
// (loaded by name at the first request, so nothing is linked against it).
//
// What a request may do, always: https only, plain http only to localhost / 127.0.0.1; no redirect is followed (a 3xx is the answer); no
// cookies are kept or sent, no sign-in of any kind (no automatic Windows logon, no stored credentials); TLS 1.2 or later. Each step
// (name lookup, connect, send, receive) has a time limit; a request that runs past it has no answer.
#pragma once
#include <cstddef>
#include <functional>
#include <string>

namespace nxrt::http {

struct Request {
    std::string method = "GET";       // "GET" or "POST"
    std::string url;                  // https://host[:port]/path?query (UTF-8)
    std::string body;                 // sent as is with content_type (POST); empty: none
    std::string content_type = "application/json";
    std::string user_agent;           // empty: "nxrt"
    size_t max_answer = 256 << 10;    // an answer body longer than this counts as no answer
    int step_timeout_ms = 10000;      // per step: lookup, connect, send, receive
};

// status: the HTTP status, or 0 when there was no answer (refused, unreachable, timed out, too long). body: the answer's body (empty with 0).
using DoneFn = std::function<void(int status, std::string body)>;

inline constexpr int kMaxInFlight = 16;   // requests running at once, for the whole process

// Starts the request on a worker: true when it was started (done is then called once, from that worker), false when not (a URL the rules
// above refuse, a method other than GET/POST, kMaxInFlight already running, no worker): done is then never called.
bool start(Request r, DoneFn done);

}  // namespace nxrt::http
