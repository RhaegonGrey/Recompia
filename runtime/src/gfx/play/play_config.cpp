// SPDX-License-Identifier: GPL-3.0-or-later
// Play mode: factory settings and the settings file (reader and writer). The file format is described in play_types.h.
#include "play_config_internal.h"

#include <sstream>
#include <string>
#include <vector>

#include "nxrt/game.h"

namespace nxrt::play {
namespace cfg {

// The settings with the built-in tables only: the outputs from ActInfo, the struct defaults, and no bindings.
Settings builtin_settings() {
    Settings s;
    for (int i = 0; i < kActCount; ++i) {
        Output o;
        parse_output(act_info(static_cast<Act>(i)).default_out, &o, nullptr);   // our own table, always valid
        s.out[i] = o;
    }
    return s;
}

// The core reader. Applies `text` on top of `*s`. Only writes what it parsed into `*s`; the caller decides what to keep.
// If the text has no [bindings.*] section and with_default_bindings is set, the factory bindings are loaded first.
bool parse_onto(const std::string& text, Settings* s, std::string* err, std::vector<std::string>* warnings, bool with_default_bindings) {
    const std::vector<std::string> lines = split_lines(text);

    // Any [bindings.*] section means the file is the whole truth about bindings, so the defaults never apply.
    bool has_bindings = false;
    for (const std::string& raw : lines) {
        const std::string l = trim(raw);
        if (!l.empty() && l[0] == '[' && lower(l).compare(0, 10, "[bindings.") == 0) has_bindings = true;
    }
    if (has_bindings) {
        s->bindings.clear();
    } else if (with_default_bindings) {
        Settings factory = builtin_settings();
        std::string ferr;
        // The factory text has bindings sections, so this inner call never comes back here.
        if (!parse_onto(default_settings_text(), &factory, &ferr, nullptr, false)) {
            if (err) *err = "internal: factory bindings do not parse: " + ferr;
            return false;
        }
        s->bindings = factory.bindings;
    }

    auto bail = [&](size_t i, const std::string& m) {
        if (err) *err = "line " + std::to_string(i + 1) + ": " + m;
    };
    auto warn = [&](size_t i, const std::string& m) {
        if (warnings) warnings->push_back("line " + std::to_string(i + 1) + ": " + m);
    };

    Sec sec = Sec::None;
    const SettingSection* fixed = nullptr;   // the section, when sec is Fixed
    Ctx ctx = Ctx::All;
    // The table rows the file set, for the fallback rule after the loop (SettingDef::fallback: right_deadzone takes deadzone's value when
    // only deadzone is there).
    std::vector<bool> set_rows(settings_table().size(), false);
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string line = trim(lines[i]);
        if (line.empty()) continue;

        if (line[0] == '#' || line[0] == ';') {
            // Full-line comment. Exception: in a bindings section the semicolon key (";") is a real binding; a line whose key
            // before the last '=' parses as an input is kept.
            bool binding_line = false;
            if (sec == Sec::Bindings) {
                const size_t eq = line.rfind('=');
                InputId probe;
                binding_line = eq != kNpos && parse_input(trim(line.substr(0, eq)), &probe);
            }
            if (!binding_line) continue;
        } else {
            line = strip_inline_comment(line);
        }

        if (line[0] == '[') {
            if (line.back() != ']') {
                bail(i, "section header without ']'");
                return false;
            }
            const std::string name = trim(line.substr(1, line.size() - 2));
            const std::string key = lower(name);
            if (key.compare(0, 9, "bindings.") == 0 && parse_ctx(name.substr(9), &ctx)) sec = Sec::Bindings;
            else if (key.compare(0, 9, "bindings.") == 0 && retired_ctx(name.substr(9))) sec = Sec::Retired;
            else if ((fixed = find_section(key)) != nullptr) sec = Sec::Fixed;
            else if (retired_section(key)) sec = Sec::Retired;
            else {
                sec = Sec::Unknown;
                warn(i, "unknown section '[" + name + "]'");
            }
            continue;
        }

        if (sec == Sec::Unknown || sec == Sec::Retired) continue;   // the keys of an unknown or retired section are skipped

        // Bindings keep the key before the LAST '=' (the key may itself be "="); their value never contains '='.
        const size_t eq = sec == Sec::Bindings ? line.rfind('=') : line.find('=');
        if (eq == kNpos) {
            bail(i, "expected 'key = value', got '" + line + "'");
            return false;
        }
        const std::string key = trim(line.substr(0, eq));
        const std::string val = trim(line.substr(eq + 1));
        if (sec == Sec::None) {
            warn(i, "'" + key + "' is outside any section");
            continue;
        }

        if (sec == Sec::Bindings) {
            InputId id;
            if (!parse_input(key, &id)) {
                bail(i, "unknown input '" + key + "'");
                return false;
            }
            // value: action [trigger [ms]]
            std::vector<std::string> tok;
            std::istringstream words(val);
            for (std::string w; words >> w;) tok.push_back(w);
            if (tok.empty() || tok.size() > 3) {
                bail(i, "expected '<action> [trigger [ms]]', got '" + val + "'");
                return false;
            }
            Binding b;
            b.in = id;
            b.ctx = ctx;
            if (const char* now = replaced_act(tok[0])) tok[0] = now;   // confirm / back of an older file: the button they pressed
            if (retired_act(tok[0])) continue;   // an action that no longer exists and pressed nothing: the binding is dropped
            if (!parse_act(tok[0], &b.act)) {
                bail(i, "unknown action '" + tok[0] + "'");
                return false;
            }
            if (tok.size() >= 2 && !parse_trig(tok[1], &b.trig)) {
                bail(i, "unknown trigger '" + tok[1] + "'");
                return false;
            }
            if (tok.size() == 3) {
                long long ms = 0;
                if (!to_int(tok[2], &ms) || ms < 1 || ms > 5000) {
                    bail(i, "ms must be 1..5000, got '" + tok[2] + "'");
                    return false;
                }
                b.ms = static_cast<int>(ms);
            }
            s->bindings.push_back(b);
            continue;
        }

        bool known = false;
        std::string msg;
        int row = -1;
        const std::string label = std::string("[") + fixed->name + "]";
        // A bad value of a fixed key keeps that key's default and the rest of the file (#218: one typo threw the whole file away,
        // and the next save wrote the factory settings over it). The setters write only a value that passed.
        if (!apply_fixed(*fixed, lower(key), val, s, &known, &msg, &row)) {
            warn(i, msg + " (" + label + " " + key + " keeps its default)");
            continue;
        }
        if (!known) warn(i, "unknown key '" + key + "' in " + label);
        if (row >= 0) set_rows[row] = true;
    }

    // A key added later than another that covered it (right_deadzone after deadzone, which was the only dead zone; right_stick_max after
    // stick_max, which only ever covered the left stick): a file that sets the older key and not the newer one gives the newer the same value.
    const std::span<const SettingDef> table = settings_table();
    for (size_t r = 0; r < table.size(); ++r) {
        if (!table[r].fallback || set_rows[r]) continue;
        const int from = find_setting(table[r].section, table[r].fallback);
        if (from >= 0 && set_rows[from]) setting_copy(table[from], table[r], s);
    }

    if (has_bindings) {
        // A file with bindings written by an older revision gets the newer factory bindings it lacks (see GameDefaults).
        const int file_rev = s->defaults_rev;   // 0 when the file has no [meta]
        if (file_rev < defaults_rev()) {
            std::string added;
            apply_default_additions(s, file_rev, &added);
            apply_default_changes(s, file_rev, &added);
            s->defaults_rev = defaults_rev();
            s->migrated = true;
            s->migration_added = added;
        }
    } else if (with_default_bindings) {
        s->defaults_rev = defaults_rev();   // the factory bindings were applied, so the result is at the current revision
    }
#if defined(__APPLE__)
    // Display defaults revision 1 (macOS): vsync on. With it off the swapchain presents immediately and Metal shows frames mid-refresh:
    // the game tore and flashed partly drawn frames on a MacBook with the frame time swinging between 8 and 50 ms. Applied once to the
    // factory text and to an older file (which is then saved); a player who turns vsync off afterwards keeps it off.
    if (s->display_rev < 1) {
        if (!s->display.vsync) {
            s->display.vsync = true;
            if (has_bindings) {
                s->migrated = true;
                s->migration_added += s->migration_added.empty() ? "vsync on (macOS)" : ", vsync on (macOS)";
            }
        }
        s->display_rev = 1;
    }
#endif
    return true;
}

}  // namespace cfg

using namespace cfg;

bool parse_settings(const std::string& text, Settings* out, std::string* err, std::vector<std::string>* warnings) {
    Settings s = builtin_settings();
    std::string perr;
    if (!parse_onto(text, &s, &perr, warnings, true)) {
        if (err) *err = perr;
        return false;
    }
    *out = s;
    return true;
}

}  // namespace nxrt::play
