// SPDX-License-Identifier: GPL-3.0-or-later
// Small text helpers for the whole runtime (ini and environment values, names, log dumps). Header-only, no locale: lower() touches ASCII letters
// only and trim() strips the C-locale white space (space, tab, newline, vertical tab, form feed, carriage return).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace nxrt::text {

inline bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }

// ASCII lower case.
inline std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    return s;
}

// ASCII upper case.
inline std::string upper(std::string s) {
    for (char& ch : s) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
    return s;
}

// s without leading and trailing white space.
inline std::string_view trim(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}
inline std::string trim(const std::string& s) { return std::string(trim(std::string_view(s))); }

inline std::string lower_trim(const std::string& s) { return lower(trim(s)); }

// Strips leading and trailing spaces and tabs only, in place.
inline void trim_blanks(std::string& s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '	')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '	')) s.erase(s.begin());
}

// The first max_bytes bytes of b as hex ("|" every 16 bytes, "..." when cut). For the Debug lines that dump a payload.
inline std::string hex_head(const std::vector<uint8_t>& b, size_t max_bytes) {
    static const char kHex[] = "0123456789abcdef";
    const size_t n = b.size() < max_bytes ? b.size() : max_bytes;
    std::string s;
    s.reserve(n * 3 + n / 16 * 2 + 4);
    for (size_t i = 0; i < n; ++i) {
        if (i) s += (i % 16) ? " " : " | ";
        s += kHex[b[i] >> 4];
        s += kHex[b[i] & 15];
    }
    if (n < b.size()) s += " ...";
    return s;
}

// The largest n' <= n that does not cut inside a UTF-8 sequence of s (n itself when s[n] starts a character or n >= s.size()).
inline size_t utf8_cut(const std::string& s, size_t n) {
    while (n && n < s.size() && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) --n;
    return n;
}

// UTF-8 to UTF-16 (Windows' wide calls; wchar_t is 16 bits there). A byte that does not start a valid, shortest-form sequence becomes U+FFFD
// and decoding goes on at the next byte.
inline std::wstring widen(std::string_view s) {
    static constexpr uint32_t kMin[5] = {0, 0, 0x80, 0x800, 0x10000};   // the smallest code point of each length (no overlong forms)
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        const size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1Fu) : n == 3 ? (c & 0x0Fu) : (c & 0x07u);
        bool ok = n != 0 && i + n <= s.size();
        for (size_t k = 1; ok && k < n; ++k) {
            const uint8_t d = static_cast<uint8_t>(s[i + k]);
            ok = (d & 0xC0) == 0x80;
            cp = (cp << 6) | (d & 0x3Fu);
        }
        if (!ok || cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out += static_cast<wchar_t>(0xFFFD);
            ++i;
            continue;
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out += static_cast<wchar_t>(0xD800 + (cp >> 10));
            out += static_cast<wchar_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += static_cast<wchar_t>(cp);
        }
        i += n;
    }
    return out;
}

}  // namespace nxrt::text
