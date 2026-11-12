// SPDX-License-Identifier: GPL-3.0-or-later
// Shared pieces of the service library: registration, events, shared memory, the title profile and the host folders.
#define _SILENCE_CXX20_U8PATH_DEPRECATION_WARNING
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>

#include "nxrt/config.h"
#include "nxrt/host_device.h"
#include "nxrt/game.h"
#include "nxrt/ipc.h"
#include "nxrt/kernel.h"
#include "nxrt/log.h"
#include "nxrt/memory.h"
#include "nxrt/services.h"
#include "nxrt/text.h"
#include "services_internal.h"

#include <windows.h>

namespace nxrt::services {

namespace {
std::mutex g_mu;
std::set<std::string>& names() {
    static std::set<std::string> s;
    return s;
}
}  // namespace

void add(const std::string& name, ipc::ServiceFactory f) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        names().insert(name);
    }
    ipc::register_service(name, std::move(f));
}

std::shared_ptr<ipc::Service> make_iface(const char* name, const std::function<void(ipc::Service&)>& setup) {
    auto s = std::make_shared<ipc::Service>(name);
    setup(*s);
    return s;
}

void register_all() {
    register_fs();
    register_settings();
    register_time();
    register_power();
    register_logfault();
    register_am();
    register_acc();
    register_hid();
    register_pl();
    register_network();
    register_misc();
    register_audio();
    register_vi_nv();
}

void reset_all() {
    fs_reset();
    settings_reset();
    time_reset();
    power_reset();
    logfault_reset();
    am_reset();
    acc_reset();
    hid_reset();
    pl_reset();
    network_reset();
    misc_reset();
    audio_reset();
    vi_nv_reset();
    profile_reload();
}

std::vector<std::string> registered_names() {
    std::lock_guard<std::mutex> lk(g_mu);
    return std::vector<std::string>(names().begin(), names().end());
}

// ---- ServiceEvent --------------------------------------------------------------------------------------------------------------

ServiceEvent::ServiceEvent(bool signaled) : st_(std::make_shared<kernel::EventState>()) { st_->signaled = signaled; }

uint32_t ServiceEvent::new_handle() const {
    auto e = std::make_shared<kernel::KEvent>();
    e->state = st_;
    e->writable = false;
    return kernel::handle_add(std::move(e));
}

void ServiceEvent::signal() {
    st_->signaled = true;
    kernel::notify_waitable_changed();
}

void ServiceEvent::clear() { st_->signaled = false; }

bool ServiceEvent::signaled() const { return st_->signaled; }

// ---- ServiceSharedMemory ---------------------------------------------------------------------------------------------------------

ServiceSharedMemory::ServiceSharedMemory(uint64_t size, const char* name) {
    const uint64_t rounded = (size + mem::kPage - 1) & ~(mem::kPage - 1);
    const uint64_t host = mem::alloc_kernel(rounded);
    if (!host) {
        NXLOG_E("services", "shared memory '%s' (%llu bytes): kernel region exhausted", name, static_cast<unsigned long long>(size));
        return;
    }
    obj_ = std::make_shared<kernel::KSharedMemory>();
    obj_->size = rounded;
    obj_->host = host;
    obj_->name = name;
}

uint32_t ServiceSharedMemory::new_handle() const { return obj_ ? kernel::handle_add(obj_) : 0; }

// ---- title profile ---------------------------------------------------------------------------------------------------------------

namespace {

struct LangName {
    const char* name;
    Language lang;
    const char* code;
};
constexpr LangName kLangs[] = {
    {"Japanese", Language::Japanese, "ja"},
    {"AmericanEnglish", Language::AmericanEnglish, "en-US"},
    {"French", Language::French, "fr"},
    {"German", Language::German, "de"},
    {"Italian", Language::Italian, "it"},
    {"Spanish", Language::Spanish, "es"},
    {"Chinese", Language::Chinese, "zh-CN"},
    {"Korean", Language::Korean, "ko"},
    {"Dutch", Language::Dutch, "nl"},
    {"Portuguese", Language::Portuguese, "pt"},
    {"Russian", Language::Russian, "ru"},
    {"Taiwanese", Language::Taiwanese, "zh-TW"},
    {"BritishEnglish", Language::BritishEnglish, "en-GB"},
    {"CanadianFrench", Language::CanadianFrench, "fr-CA"},
    {"LatinAmericanSpanish", Language::LatinAmericanSpanish, "es-419"},
    {"SimplifiedChinese", Language::SimplifiedChinese, "zh-Hans"},
    {"TraditionalChinese", Language::TraditionalChinese, "zh-Hant"},
    {"BrazilianPortuguese", Language::BrazilianPortuguese, "pt-BR"},
};

using text::lower;
using text::trim;

bool parse_language(const std::string& s, Language& out) {
    const std::string l = lower(trim(s));
    for (const LangName& n : kLangs)
        if (lower(n.name) == l || lower(n.code) == l) {
            out = n.lang;
            return true;
        }
    return false;
}

int parse_region(const std::string& s, int def) {
    const std::string l = lower(trim(s));
    if (l.empty()) return def;
    if (std::isdigit(static_cast<unsigned char>(l[0]))) return static_cast<int>(std::strtol(l.c_str(), nullptr, 0));
    static const char* kNames[] = {"japan", "usa", "europe", "australia", "hongkongtaiwankorea", "china"};
    for (int i = 0; i < 6; ++i)
        if (l == kNames[i]) return i;
    if (l == "korea" || l == "taiwan" || l == "hongkong") return 4;
    if (l == "us") return 1;
    return def;
}

// The Windows display language as the closest Switch language, or false when Windows uses a language the Switch has none for.
bool windows_language(Language& out) {
    const LANGID id = GetUserDefaultUILanguage();
    const WORD sub = SUBLANGID(id);
    switch (PRIMARYLANGID(id)) {
    case LANG_JAPANESE: out = Language::Japanese; return true;
    case LANG_ENGLISH:
        out = (sub == SUBLANG_ENGLISH_US || sub == SUBLANG_ENGLISH_CAN) ? Language::AmericanEnglish : Language::BritishEnglish;
        return true;
    case LANG_FRENCH: out = sub == SUBLANG_FRENCH_CANADIAN ? Language::CanadianFrench : Language::French; return true;
    case LANG_GERMAN: out = Language::German; return true;
    case LANG_ITALIAN: out = Language::Italian; return true;
    case LANG_SPANISH:
        out = (sub == SUBLANG_SPANISH || sub == SUBLANG_SPANISH_MODERN) ? Language::Spanish : Language::LatinAmericanSpanish;
        return true;
    case LANG_KOREAN: out = Language::Korean; return true;
    case LANG_DUTCH: out = Language::Dutch; return true;
    case LANG_PORTUGUESE: out = sub == SUBLANG_PORTUGUESE_BRAZILIAN ? Language::BrazilianPortuguese : Language::Portuguese; return true;
    case LANG_RUSSIAN: out = Language::Russian; return true;
    case LANG_CHINESE:
        out = (sub == SUBLANG_CHINESE_TRADITIONAL || sub == SUBLANG_CHINESE_HONGKONG || sub == SUBLANG_CHINESE_MACAU)
                  ? Language::TraditionalChinese : Language::SimplifiedChinese;
        return true;
    default: return false;
    }
}

// A language the title supports: the wanted one, else its nearest sibling the title has (British for American English and so on), else
// false.
bool supported_or_sibling(Language want, uint32_t mask, Language& out) {
    auto has = [mask](Language l) { return (mask >> static_cast<uint32_t>(l)) & 1u; };
    if (has(want)) return out = want, true;
    Language alt;
    switch (want) {
    case Language::AmericanEnglish: alt = Language::BritishEnglish; break;
    case Language::BritishEnglish: alt = Language::AmericanEnglish; break;
    case Language::French: alt = Language::CanadianFrench; break;
    case Language::CanadianFrench: alt = Language::French; break;
    case Language::Spanish: alt = Language::LatinAmericanSpanish; break;
    case Language::LatinAmericanSpanish: alt = Language::Spanish; break;
    case Language::Portuguese: alt = Language::BrazilianPortuguese; break;
    case Language::BrazilianPortuguese: alt = Language::Portuguese; break;
    case Language::SimplifiedChinese: alt = Language::Chinese; break;
    case Language::Chinese: alt = Language::SimplifiedChinese; break;
    case Language::TraditionalChinese: alt = Language::Taiwanese; break;
    case Language::Taiwanese: alt = Language::TraditionalChinese; break;
    default: return false;
    }
    if (has(alt)) return out = alt, true;
    return false;
}

// The player's pick in the play-mode settings file ([tweaks] language of controls.ini, set from the launcher), or "" when there is none. Read
// here with the plain ini reader so the language is known before the play-mode settings are loaded.
std::string controls_language() {
    const std::string rel = config_get_str("play", "controls", "controls.ini");
    char full[MAX_PATH];
    if (!GetFullPathNameA(rel.c_str(), MAX_PATH, full, nullptr)) return {};
    char buf[64] = {};
    GetPrivateProfileStringA("tweaks", "language", "", buf, sizeof buf, full);
    return trim(std::string(buf));
}

uint32_t parse_language_mask(const std::string& s, uint32_t def) {
    const std::string t = trim(s);
    if (t.empty()) return def;
    if (std::isdigit(static_cast<unsigned char>(t[0]))) return static_cast<uint32_t>(std::strtoul(t.c_str(), nullptr, 0));
    uint32_t m = 0;
    size_t pos = 0;
    while (pos <= t.size()) {
        size_t comma = t.find(',', pos);
        if (comma == std::string::npos) comma = t.size();
        Language l;
        if (parse_language(t.substr(pos, comma - pos), l)) m |= 1u << static_cast<uint32_t>(l);
        pos = comma + 1;
    }
    return m ? m : def;
}

bool parse_uid(const std::string& s, uint64_t& hi, uint64_t& lo) {
    std::string h;
    for (char c : s)
        if (std::isxdigit(static_cast<unsigned char>(c))) h += c;
    if (h.size() != 32) return false;
    hi = std::strtoull(h.substr(0, 16).c_str(), nullptr, 16);
    lo = std::strtoull(h.substr(16).c_str(), nullptr, 16);
    return true;
}

// The defaults before the ini: the registered game's own facts (nxrt/game.h, Info::title_defaults), else the neutral ones of the struct.
TitleProfile game_defaults() {
    TitleProfile p;
    if (const game::Info* g = game::current(); g && g->title_defaults) g->title_defaults(p);
    if (p.supported_languages == 0) p.supported_languages = 1u << static_cast<uint32_t>(Language::AmericanEnglish);
    return p;
}

TitleProfile load_profile() {
    TitleProfile p = game_defaults();
    const uint64_t game_owner = p.save_data_owner_id;      // 0: the save data owner is the program id
    p.program_id = static_cast<uint64_t>(config_get_int("title", "program_id", static_cast<int64_t>(p.program_id)));
    // [process] program_id (kernel GetInfo ProgramId) overrides when it is set explicitly.
    if (config_has("process", "program_id")) p.program_id = static_cast<uint64_t>(config_get_int("process", "program_id", 0));
    p.name = config_get_str("title", "name", p.name.c_str());
    p.display_version = config_get_str("title", "display_version", p.display_version.c_str());
    p.supported_languages = parse_language_mask(config_get_str("title", "supported_languages", ""), p.supported_languages);
    p.save_data_owner_id = static_cast<uint64_t>(config_get_int("title", "save_owner_id", static_cast<int64_t>(game_owner ? game_owner : p.program_id)));
    p.account_save_size = static_cast<uint64_t>(config_get_int("title", "account_save_size", static_cast<int64_t>(p.account_save_size)));
    p.account_save_journal = static_cast<uint64_t>(config_get_int("title", "account_save_journal", static_cast<int64_t>(p.account_save_journal)));
    p.device_save_size = static_cast<uint64_t>(config_get_int("title", "device_save_size", static_cast<int64_t>(p.device_save_size)));
    p.device_save_journal = static_cast<uint64_t>(config_get_int("title", "device_save_journal", static_cast<int64_t>(p.device_save_journal)));
    p.cache_save_size = static_cast<uint64_t>(config_get_int("title", "cache_save_size", static_cast<int64_t>(p.cache_save_size)));
    p.cache_save_journal = static_cast<uint64_t>(config_get_int("title", "cache_save_journal", static_cast<int64_t>(p.cache_save_journal)));
    p.main_thread_stack = static_cast<uint64_t>(config_get_int("title", "main_thread_stack", static_cast<int64_t>(p.main_thread_stack)));
    p.main_thread_priority = static_cast<int>(config_get_int("title", "main_thread_priority", p.main_thread_priority));
    p.main_thread_core = static_cast<int>(config_get_int("title", "main_thread_core", p.main_thread_core));
    p.system_resource_size = static_cast<uint64_t>(config_get_int("title", "system_resource_size", static_cast<int64_t>(p.system_resource_size)));

    // Language: [system] language of the game ini, else the player's pick in controls.ini, else the Windows display language; "auto" or empty
    // passes to the next. A language the title lacks falls to its nearest sibling, else American English.
    {
        std::string from = "[system] language";
        std::string want = trim(config_get_str("system", "language", ""));
        if (want.empty() || lower(want) == "auto") want = controls_language(), from = "controls.ini [tweaks] language";
        Language l = Language::AmericanEnglish, pick;
        bool named = !want.empty() && lower(want) != "auto" && parse_language(want, l);
        if (!named) from = windows_language(l) ? "Windows" : "default";
        if (supported_or_sibling(l, p.supported_languages, pick)) p.language = pick;
        else p.language = Language::AmericanEnglish;
        if (!named && !want.empty() && lower(want) != "auto") NXLOG_W("services", "language '%s' is not a language name; using %s", want.c_str(), from.c_str());
        NXLOG_I("services", "language %s (from %s)", language_code(p.language), from.c_str());
    }
    p.region = parse_region(config_get_str("system", "region", ""), p.region);
    p.firmware = config_get_str("system", "firmware", p.firmware.c_str());
    p.device_nickname = config_get_str("system", "nickname", p.device_nickname.c_str());
    // [am] operation_mode = auto (default) | docked | handheld. auto: handheld on a handheld PC (host_is_handheld), docked elsewhere.
    // In handheld mode the game picks its own lighter graphics settings, as on the console's own screen: in game on the PC ~14% more
    // frames, on a Legion Go S (featureJosh's fork) 50-57 -> 62-69 fps.
    {
        const std::string m = lower(config_get_str("am", "operation_mode", "auto"));
        p.docked = m == "auto" ? !host_is_handheld() : m != "handheld";
        NXLOG_I("am", "operation mode: %s ([am] operation_mode = %s%s)", p.docked ? "docked" : "handheld", m.c_str(),
                m == "auto" ? (p.docked ? ", not a handheld" : ", a handheld") : "");
    }
    p.user_nickname = config_get_str("account", "nickname", p.user_nickname.c_str());
    uint64_t hi, lo;
    if (parse_uid(config_get_str("account", "uid", ""), hi, lo)) {
        p.user_uid_hi = hi;
        p.user_uid_lo = lo;
    }
    return p;
}

std::mutex g_prof_mu;
TitleProfile g_prof;
bool g_prof_loaded = false;

}  // namespace

const TitleProfile& profile() {
    std::lock_guard<std::mutex> lk(g_prof_mu);
    if (!g_prof_loaded) {
        g_prof = load_profile();
        g_prof_loaded = true;
    }
    return g_prof;
}

uint64_t program_id_now() {
    if (config_has("process", "program_id")) return static_cast<uint64_t>(config_get_int("process", "program_id", 0));
    return static_cast<uint64_t>(config_get_int("title", "program_id", static_cast<int64_t>(game_defaults().program_id)));
}

void profile_reload() {
    std::lock_guard<std::mutex> lk(g_prof_mu);
    g_prof = load_profile();
    g_prof_loaded = true;
}

const char* language_code(Language l) {
    for (const LangName& n : kLangs)
        if (n.lang == l) return n.code;
    return "en-US";
}

uint64_t language_code_u64(Language l) {
    const char* c = language_code(l);
    uint64_t v = 0;
    for (size_t i = 0; i < 8 && c[i]; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(c[i])) << (8 * i);
    return v;
}

std::string romfs_dir() { return config_get_str("fs", "romfs", "game/romfs"); }
std::string romfs_base_dir() { return config_get_str("fs", "romfs_base", ""); }
std::string romfs_mod_dir() { return config_get_str("fs", "romfs_mod", ""); }

const std::vector<AddOnContent>& add_on_contents() {
    static std::once_flag once;
    static std::vector<AddOnContent> list;
    std::call_once(once, [] {
        const std::string dir = config_get_str("fs", "aoc_dir", "");
        if (dir.empty()) return;
        const uint64_t base = profile().program_id + 0x1000;   // the add-on base id, as aoc:u GetAddOnContentBaseId answers
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(std::filesystem::u8path(dir), ec)) {
            const auto u8name = e.path().filename().u8string();
            const std::string name(u8name.begin(), u8name.end());
            const std::filesystem::path romfs = e.path() / "romfs";
            if (name.size() != 16 || !std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isxdigit(c); }) ||
                !std::filesystem::is_directory(romfs, ec))
                continue;
            const uint64_t id = std::strtoull(name.c_str(), nullptr, 16);
            if ((id & ~0xFFFull) != base || id == base) {
                NXLOG_W("aoc", "[fs] aoc_dir: %s is not an add-on of this title (base id 0x%016llX): ignored", name.c_str(),
                        static_cast<unsigned long long>(base));
                continue;
            }
            const auto u8romfs = romfs.u8string();
            list.push_back({id, static_cast<int32_t>(id - base), std::string(u8romfs.begin(), u8romfs.end())});
        }
        if (ec) NXLOG_W("aoc", "[fs] aoc_dir '%s' cannot be read: %s", dir.c_str(), ec.message().c_str());
        std::sort(list.begin(), list.end(), [](const AddOnContent& a, const AddOnContent& b) { return a.id < b.id; });
        for (const AddOnContent& a : list)
            NXLOG_I("aoc", "add-on content %d (0x%016llX) installed from %s", a.index, static_cast<unsigned long long>(a.id), a.romfs.c_str());
    });
    return list;
}

std::string save_dir() {
    std::string d = config_get_str("fs", "save_dir", "");
#if !defined(_WIN32)
    // A configured Windows path would split differently in std::filesystem (backslash is a literal character) and in the
    // shim's CreateFileW (backslash is a separator): the folder created and the folder opened were two different ones.
    for (char& c : d)
        if (c == '\\') c = '/';
#endif
    if (!d.empty()) return d;
    const char* folder = game::info().save_folder;
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    return std::string(appdata && *appdata ? appdata : ".") + "\\" + (folder && *folder ? folder : "nxrt") + "\\save";
#else
    // The Windows default is %APPDATA%\<folder>\save; macOS keeps the same data under Application Support.
    const char* home = std::getenv("HOME");
    return std::string(home && *home ? home : ".") + "/Library/Application Support/" + (folder && *folder ? folder : "nxrt") + "/save";
#endif
}

}  // namespace nxrt::services
