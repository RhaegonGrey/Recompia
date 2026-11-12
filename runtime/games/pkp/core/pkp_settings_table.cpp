// SPDX-License-Identifier: GPL-3.0-or-later
// Pokémon Pokopia: spec() and the doc rows of the [pkp] table (pkp_settings_table.h). Pure data, in nxrt_game_pkp_data, so the docs
// generator (apps/settings_doc.cpp) reads it without the runtime; the getters are in pkp_settings.cpp.
#include "core/pkp_settings_table.h"

namespace nxrt::games::pkp::settings {

// A gameplay mod's switch is a bool on by default unless the repo owner made it opt_in; in a release build (PKP_GAMEPLAY_DEFAULTS_CHECK,
// set by game-build with PKP_RELEASE: every build on GitHub, stable and testing) that holds for its default too, so a switch turned off
// for a test build cannot ship.
#if PKP_GAMEPLAY_DEFAULTS_CHECK
static_assert(table::switches_default_on(),
              "release build: a gameplay mod switch ([pkp] key marked gameplay(), not opt_in()) defaults off; turn it back on");
#endif

const Spec& spec(Key k) { return table::at(k); }

std::vector<setting_doc::Row> doc_rows() {
    std::vector<setting_doc::Row> rows;
    for (const Spec& sp : table::kSpecs) {
        setting_doc::Row r;
        r.section = kSection;
        r.key = sp.name;
        r.optional = sp.optional;
        const bool ranged = sp.lo < sp.hi;
        switch (sp.type) {
            case Type::Bool:
                r.type = "bool";
                r.def = sp.def != 0 ? "1" : "0";
                break;
            case Type::Int:
            case Type::Double:
                r.type = sp.type == Type::Int ? "int" : "real";
                r.def = setting_doc::number_text(sp.def);
                r.has_min = r.has_max = r.clamped = ranged;
                r.min = sp.lo;
                r.max = sp.hi;
                break;
            case Type::String: {
                r.def = sp.def_str;
                for (const char* p = sp.choices; *p;) {
                    while (*p == ' ') ++p;
                    const char* e = p;
                    while (*e && *e != ' ') ++e;
                    if (e != p) r.choices.emplace_back(p, e);
                    p = e;
                }
                r.type = r.choices.empty() ? "text" : "choice";
                break;
            }
        }
        if (r.optional) r.def.clear();
        if (sp.gameplay) r.group = "gameplay";
        if (sp.parent != Key::Count) r.parent = table::at(sp.parent).name;
        r.opt_in = sp.opt_in;
        r.hidden = sp.hidden;
        r.doc = sp.doc;
        if (sp.applies == Applies::AtStart) r.doc += " Next start.";
        rows.push_back(std::move(r));
    }
    return rows;
}

}  // namespace nxrt::games::pkp::settings