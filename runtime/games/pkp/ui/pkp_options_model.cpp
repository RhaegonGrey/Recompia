// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options, StaticTab (pkp_options_model.h): the constexpr Row tables as tabs. A row's value is a play-mode setting (Row::get,
// Row::set; the changes of every tab reach play::apply_settings once, through Apply) or a [pkp] key (Row::key, saved with settings::set_*
// when the screen commits).
#include "ui/pkp_options_model.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "core/pkp_settings.h"
#include "nxrt/extras.h"
#include "play_game.h"

namespace nxrt::games::pkp::options_detail {
namespace {

int row_hi(const Row& r) { return r.choices ? r.choices - 1 : r.max ? r.max() : r.hi; }

bool is_key_row(const Row& r) { return r.key != settings::Key::Count; }
int key_position(const Row& r, double v) { return static_cast<int>(std::lround((v - settings::spec(r.key).lo) / r.unit)); }

// The position of the value in force.
int row_get(const Row& r, const S& s) {
    if (!is_key_row(r)) return r.get(s);
    switch (settings::spec(r.key).type) {
        case settings::Type::Bool: return settings::get_bool(r.key) ? 1 : 0;
        case settings::Type::Int: return key_position(r, static_cast<double>(settings::get_int(r.key)));
        case settings::Type::Double: return key_position(r, settings::get_double(r.key));
        case settings::Type::String: break;   // no row kind (key_row)
    }
    return 0;
}

// A [pkp] row's position, saved: the game ini, and the getters at once.
void save_key(const Row& r, int pos) {
    const settings::Spec& sp = settings::spec(r.key);
    switch (sp.type) {
        case settings::Type::Bool: settings::set_bool(r.key, pos != 0); break;
        case settings::Type::Int: settings::set_int(r.key, std::llround(sp.lo + pos * r.unit)); break;
        case settings::Type::Double: settings::set_double(r.key, sp.lo + pos * r.unit); break;
        case settings::Type::String: break;
    }
}

}  // namespace

Shape StaticTab::shape(int i) const {
    const Row& r = rows_[i];
    const Use use = r.capture ? Use::Capture : r.live ? Use::Live : Use::Value;
    return Shape{r.choices, r.lo, row_hi(r), use};
}

const char* StaticTab::value_text(int i, int pos, char* out, size_t n) const {
    const Row& r = rows_[i];
    if (r.choices) return r.names[std::clamp(pos, 0, r.choices - 1)];
    if (r.capture) return capture::text(*r.capture, out, n);
    if (r.text) return r.text(pos, out, n);
    if (r.list) return r.list[std::clamp(pos - r.lo, 0, r.hi - r.lo)];
    if (r.zero && pos == r.lo) return r.zero;
    std::snprintf(out, n, r.fmt, r.base + pos * r.step);
    return out;
}

// The position of the default (the record's default value).
int StaticTab::def(int i) const {
    static const S defaults = play::default_settings();
    const Row& r = rows_[i];
    if (!is_key_row(r)) return r.get(defaults);
    const settings::Spec& sp = settings::spec(r.key);
    return sp.type == settings::Type::Bool ? (sp.def != 0 ? 1 : 0) : key_position(r, sp.def);
}

void StaticTab::open() {
    if (on_open_) on_open_();
    const S s = play::current_settings();
    for (int i = 0; i < count_; ++i) cur[i] = orig[i] = std::clamp(row_get(rows_[i], s), static_cast<int>(rows_[i].lo), row_hi(rows_[i]));
}

void StaticTab::commit(Apply& a) {
    for (int i = 0; i < count_; ++i) {
        if (cur[i] == orig[i]) continue;
        if (is_key_row(rows_[i])) {
            save_key(rows_[i], cur[i]);   // the game ini, now
        } else {
            rows_[i].set(a.s, cur[i]);
            a.changed = true;
        }
    }
}

// The live rows are the picture's (Row::live): a discard puts the picture in force back.
void StaticTab::revert() {
    TabModel::revert();
    if (std::any_of(rows_, rows_ + count_, [](const Row& r) { return r.live; })) extras::set_picture_live(play::current_settings().extras.picture);
}

// A live row moved: the picture with this tab's changes so far, at once (nothing is saved).
void StaticTab::preview(int, int) {
    S s = play::current_settings();
    for (int i = 0; i < count_; ++i)
        if (cur[i] != orig[i] && !is_key_row(rows_[i])) rows_[i].set(s, cur[i]);
    extras::set_picture_live(s.extras.picture);
}

}  // namespace nxrt::games::pkp::options_detail
