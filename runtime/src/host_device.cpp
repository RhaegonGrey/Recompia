// SPDX-License-Identifier: GPL-3.0-or-later
// The machine the runtime runs on (include/nxrt/host_device.h). Based on the Steam Deck detection in tinfoil-chewer's Linux fork
// (tinfoil-chewer/pokopia-recomp-linux 8b150a4), widened to the handhelds in kBoards and to Windows.
#include "nxrt/host_device.h"

#include "nxrt/config.h"
#include "nxrt/log.h"
#include "nxrt/text.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace nxrt {

namespace {

// An SMBIOS string: HKLM\HARDWARE\DESCRIPTION\System\BIOS (Windows; Wine's wineboot fills it from SMBIOS), else the Linux DMI file
// through Wine's Z: drive (on Windows that path does not exist).
std::string smbios(const char* value, const char* dmi_file) {
    char buf[128] = {};
    DWORD n = sizeof buf;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", value, RRF_RT_REG_SZ, nullptr, buf, &n) == ERROR_SUCCESS)
        return text::lower_trim(buf);
    std::ifstream f(std::string("Z:\\sys\\class\\dmi\\id\\") + dmi_file);
    std::string s;
    if (f && std::getline(f, s)) return text::lower_trim(s);
    return {};
}

// The boards that are handhelds, matched on the lower-cased SMBIOS product name or family. A prefix match unless exact. A name that
// covers both AMD and Intel models is left out; the string of each row is in the comment beside it.
struct BoardRule {
    bool family;        // matched against product_family, else product_name
    bool exact;
    const char* text;
};
constexpr BoardRule kBoards[] = {
    {false, true, "jupiter"},          // Steam Deck LCD
    {false, true, "galileo"},          // Steam Deck OLED
    {true, false, "legion go s"},      // Legion Go S: family "Legion Go S 8APU1" (its product names are SKU codes: 83L3, 83N6, ...)
    {false, false, "rog ally"},        // ROG Ally and Ally X: product "ROG Ally RC71L_RC71L" / "ROG Ally X RC72LA_RC72LA", family "ROG Ally"
    {true, false, "rog ally"},
    {false, false, "rog xbox ally"},   // ROG Xbox Ally / Ally X: product "ROG Xbox Ally RC73YA_RC73YA" (HHD; the RC74X one spells XBOX)
    {false, true, "83e1"},             // Legion Go (first model): product "83E1", version "Legion Go 8APU1" (HHD, DRM panel-orientation patch)
    {false, true, "83n0"},             // Legion Go 2: product "83N0" (Bazzite log, HHD)
    {false, true, "83n1"},             // Legion Go 2, other SKU (HHD)
    {false, false, "claw a8"},         // MSI Claw A8 (Ryzen Z2 Extreme): product "Claw A8 BZ2EM" (CachyOS report, HHD); the other Claws are Intel
};

// The first rule the board matches, or null.
const BoardRule* match_board(const std::string& product, const std::string& family) {
    for (const BoardRule& r : kBoards) {
        const std::string& v = r.family ? family : product;
        if (r.exact ? v == r.text : v.rfind(r.text, 0) == 0) return &r;
    }
    return nullptr;
}

bool detect(std::string* why) {
    const std::string forced = text::lower_trim(config_get_str("host", "handheld", "auto"));
    if (forced == "1" || forced == "on" || forced == "true" || forced == "yes") return *why = "[host] handheld = on", true;
    if (forced == "0" || forced == "off" || forced == "false" || forced == "no") return *why = "[host] handheld = off", false;
    char env[16] = {};
    const DWORD n = GetEnvironmentVariableA("SteamDeck", env, sizeof env);
    if (n > 0 && n < sizeof env && text::lower_trim(std::string(env, n)) == "1") return *why = "Steam's SteamDeck=1", true;
    const std::string product = smbios("SystemProductName", "product_name"), family = smbios("SystemFamily", "product_family");
    if (const BoardRule* r = match_board(product, family))
        return *why = "board " + product + " / " + family + ", matched " + (r->family ? "family " : "product ") + (r->exact ? "= '" : "prefix '") + r->text + "'", true;
    *why = "not a known handheld (board '" + product + "' / '" + family + "'; [host] handheld = on forces it)";
    return false;
}

}  // namespace

bool host_is_handheld() {
    static const bool handheld = [] {
        std::string why;
        const bool h = detect(&why);
        NXLOG_I("host", "handheld: %s (%s)", h ? "yes" : "no", why.c_str());
        return h;
    }();
    return handheld;
}

bool host_is_steam_deck() {
    static const bool deck = [] {
        char env[16] = {};
        const DWORD n = GetEnvironmentVariableA("SteamDeck", env, sizeof env);
        const std::string product = smbios("SystemProductName", "product_name");
        const bool d = (n > 0 && n < sizeof env && text::lower_trim(std::string(env, n)) == "1") || product == "jupiter" || product == "galileo";
        NXLOG_I("host", "steam deck: %s (board '%s')", d ? "yes" : "no", product.c_str());
        return d;
    }();
    return deck;
}

bool host_is_wine() {
    static const bool wine = [] {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        return nt && GetProcAddress(nt, "wine_get_version");
    }();
    return wine;
}

// ---- Steam's on-screen keyboard -------------------------------------------------------------------------------------------------------

namespace {

std::string env_lower(const char* name) {
    char v[64] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof v);
    return n > 0 && n < sizeof v ? text::lower_trim(std::string(v, n)) : std::string();
}

bool file_exists(const wchar_t* path) {
    const DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Runs `exe "url"` and waits for it (up to 5 s): the commands reach Steam in the order they were asked.
bool run_with_url(const wchar_t* exe, const std::string& url) {
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" \"" + std::wstring(url.begin(), url.end()) + L"\"";   // the URL is ASCII
    STARTUPINFOW si = {sizeof si};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Hands a steam:// URL to the Steam client. Under Proton, the prefix's steam.exe is Proton's helper, which forwards steam:// arguments to
// the native client (steam_helper: steam-runtime-steam-remote, else steam); on Windows it is Steam itself. Plain Wine without Proton:
// winebrowser (xdg-open, which reaches Steam through its x-scheme-handler/steam). Last: ShellExecute.
void send_to_steam(const std::string& url) {
    static const wchar_t* const kSteamExe[] = {L"C:\\Program Files (x86)\\Steam\\steam.exe", L"C:\\windows\\system32\\steam.exe"};
    for (const wchar_t* exe : kSteamExe) {
        if (file_exists(exe) && run_with_url(exe, url)) {
            NXLOG_I("host", "Steam keyboard: %s (through %ls)", url.c_str(), exe);
            return;
        }
    }
    if (host_is_wine() && run_with_url(L"C:\\windows\\system32\\winebrowser.exe", url)) {
        NXLOG_I("host", "Steam keyboard: %s (through winebrowser)", url.c_str());
        return;
    }
    const INT_PTR r = reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (r > 32) NXLOG_I("host", "Steam keyboard: %s (ShellExecute)", url.c_str());
    else NXLOG_W("host", "Steam keyboard: %s could not be handed to Steam (no steam.exe, ShellExecute %lld)", url.c_str(), static_cast<long long>(r));
}

struct SteamQueue {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> urls;
    bool started = false;
};
SteamQueue& steam_queue() {
    static SteamQueue* q = new SteamQueue;   // never destroyed: the worker may still run at exit
    return *q;
}

// One worker, in order: an open and the close right after it must not reach Steam swapped.
void queue_url(std::string url) {
    SteamQueue& q = steam_queue();
    std::lock_guard<std::mutex> lk(q.mu);
    q.urls.push_back(std::move(url));
    if (!q.started) {
        q.started = true;
        std::thread([] {
            log_set_thread_name("steam-url");
            SteamQueue& w = steam_queue();
            for (;;) {
                std::string u;
                {
                    std::unique_lock<std::mutex> l(w.mu);
                    w.cv.wait(l, [&] { return !w.urls.empty(); });
                    u = std::move(w.urls.front());
                    w.urls.pop_front();
                }
                send_to_steam(u);
            }
        }).detach();
    }
    q.cv.notify_one();
}

}  // namespace

// ---- The facts a bug report needs -----------------------------------------------------------------------------------------------------

namespace {

// The first line of a unix file through Wine's Z: drive, trimmed; empty when the file is missing. Read only, one small virtual file.
std::string first_line(const std::string& unix_path) {
    std::string win = "Z:";
    for (const char c : unix_path) win += c == '/' ? '\\' : c;
    std::ifstream f(win);
    std::string s;
    return f && std::getline(f, s) ? text::trim(s) : std::string();
}

// " name=value" when the value is not empty, else nothing: a missing file leaves its field out.
void add(std::string& out, const char* name, const std::string& value) {
    if (!value.empty()) out += std::string(" ") + name + "=" + value;
}

// A byte count as MiB (a sysfs value in bytes, or in pages of page_bytes); empty when it is not a number.
std::string mib(const std::string& v, unsigned long long unit = 1) {
    char* end = nullptr;
    const unsigned long long n = std::strtoull(v.c_str(), &end, 10);
    return v.empty() || end == v.c_str() ? std::string() : std::to_string(n * unit / (1024 * 1024)) + "MiB";
}

// "Key:   value kB" in /proc/meminfo as MiB.
std::string meminfo_mib(const char* key) {
    std::ifstream f("Z:\\proc\\meminfo");
    std::string line;
    const std::string k = std::string(key) + ":";
    while (std::getline(f, line))
        if (line.rfind(k, 0) == 0) return mib(text::trim(line.substr(k.size())), 1024);
    return {};
}

}  // namespace

// One line, Wine/Linux only; every field is omitted when its file is missing or empty. No serial, uuid, user name, hostname or path.
void host_log_facts() {
    if (!host_is_wine()) return;
    std::string o;
    add(o, "kernel", first_line("/proc/sys/kernel/osrelease"));
    static const char* const kDmi[][2] = {{"vendor", "sys_vendor"}, {"product", "product_name"}, {"family", "product_family"},
                                          {"board", "board_name"}, {"bios", "bios_version"}};
    for (const auto& d : kDmi) add(o, d[0], first_line(std::string("/sys/class/dmi/id/") + d[1]));
    {   // the first "model name" line, and how many "processor" lines
        std::ifstream f("Z:\\proc\\cpuinfo");
        std::string line, model;
        int cpus = 0;
        for (int i = 0; i < 20000 && std::getline(f, line); ++i) {
            if (line.rfind("processor", 0) == 0) ++cpus;
            else if (model.empty() && line.rfind("model name", 0) == 0 && line.find(':') != std::string::npos) model = text::trim(line.substr(line.find(':') + 1));
        }
        add(o, "cpu", model);
        if (cpus) add(o, "cpus", std::to_string(cpus));
    }
    const std::string cpu0 = "/sys/devices/system/cpu/cpu0/cpufreq/";
    add(o, "governor", first_line(cpu0 + "scaling_governor"));
    add(o, "epp", first_line(cpu0 + "energy_performance_preference"));
    add(o, "pstate", first_line("/sys/devices/system/cpu/amd_pstate/status"));
    add(o, "boost", first_line("/sys/devices/system/cpu/cpufreq/boost"));
    for (int i = 0; i < 8; ++i) {   // the GPU: the first card whose PCI vendor is AMD
        const std::string d = "/sys/class/drm/card" + std::to_string(i) + "/device/";
        if (first_line(d + "vendor") != "0x1002") continue;
        add(o, "gpu", first_line(d + "vendor") + ":" + first_line(d + "device"));
        add(o, "vram", mib(first_line(d + "mem_info_vram_total")));
        add(o, "vis_vram", mib(first_line(d + "mem_info_vis_vram_total")));
        add(o, "gtt", mib(first_line(d + "mem_info_gtt_total")));
        break;
    }
    add(o, "ttm_pages_limit", mib(first_line("/sys/module/ttm/parameters/pages_limit"), 4096));   // pages of 4 KiB
    add(o, "ram", meminfo_mib("MemTotal"));
    add(o, "swap", meminfo_mib("SwapTotal"));
    add(o, "swappiness", first_line("/proc/sys/vm/swappiness"));
    add(o, "zram0", mib(first_line("/sys/block/zram0/disksize")));
    add(o, "gamescope", GetEnvironmentVariableA("GAMESCOPE_WAYLAND_DISPLAY", nullptr, 0) ? "yes" : "no");
    add(o, "steamdeck_env", env_lower("SteamDeck"));
    add(o, "desktop", env_lower("XDG_CURRENT_DESKTOP"));
    add(o, "proton", GetEnvironmentVariableA("STEAM_COMPAT_DATA_PATH", nullptr, 0) ? "yes" : "no");
    NXLOG_I("host", "host facts:%s", o.c_str());
}

bool host_steam_keyboard_wanted() {
    static const bool want = [] {
        const std::string c = text::lower_trim(config_get_str("applet", "steam_keyboard", "auto"));
        bool w = false;
        const char* why = "";
        if (c == "1" || c == "on" || c == "true" || c == "yes") {
            w = true;
            why = "[applet] steam_keyboard = on";
        } else if (c == "0" || c == "off" || c == "false" || c == "no") {
            why = "[applet] steam_keyboard = off";
        } else if (!host_is_wine()) {
            why = "Windows (a steam:// URL would start Steam if it is not running)";
        } else if (host_is_steam_deck()) {
            w = true;
            why = "a Steam Deck";
        } else if (env_lower("SteamGamepadUI") == "1") {
            w = true;
            why = "Steam's gamepad UI (SteamGamepadUI=1)";
        } else if (env_lower("SteamOS") == "1") {
            w = true;
            why = "SteamOS (SteamOS=1)";
        } else {
            why = "not a handheld, SteamOS or Steam's gamepad UI";
        }
        NXLOG_I("host", "Steam keyboard for text the game asks for: %s (%s)", w ? "yes" : "no", why);
        return w;
    }();
    return want;
}

void host_steam_keyboard_open(int x, int y, int w, int h) {
    // Mode 0 = single line: Steam's Enter sends Return and closes its keyboard (SDL's X11 backend asks the same way).
    char url[160];
    std::snprintf(url, sizeof url, "steam://open/keyboard?XPosition=%d&YPosition=%d&Width=%d&Height=%d&Mode=0", std::max(0, x), std::max(0, y),
                  std::max(0, w), std::max(0, h));
    queue_url(url);
}

void host_steam_keyboard_close() { queue_url("steam://close/keyboard"); }

}  // namespace nxrt
