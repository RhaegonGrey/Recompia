// SPDX-License-Identifier: GPL-3.0-or-later
// Controller remapping owns its pending per-button choices; the screen commits them with the other tabs.
#include "ui/pkp_options_model.h"
#include "pad_choices.h"
#include "play_game.h"
#include <algorithm>
#include <cstdio>
#include <iterator>

namespace nxrt::games::pkp::options_detail {
namespace {
class ControllerTab final : public StaticTab {
public:
    ControllerTab() : StaticTab(kControllerRows) {}
    Shape shape(int i) const override {
        if (i >= count_ - 2) return Shape{0, 0, 254, Use::Rows};
        return StaticTab::shape(i);
    }
    const char* value_text(int i, int pos, char* out, size_t n) const override {
        if (i < count_ - 2) return StaticTab::value_text(i, pos, out, n);
        const char* value = i == count_ - 2 ? play::kPadSrcLabels[selected_]
            : pending_[selected_] < play::kPadChoiceCount ? play::kPadChoiceNames[pending_[selected_]] : play::kPadCustomLabel;
        std::snprintf(out, n, "%s", value);
        return out;
    }
    void open() override {
        StaticTab::open();
        selected_ = 0;
        const S s = play::current_settings();
        for (int i = 0; i < play::kPadSrcCount; ++i) {
            const int c = play::pad_choice_of(s.pad.remap[i]);
            pending_[i] = saved_[i] = c < 0 ? play::kPadChoiceCount : c;
        }
    }
    void pick(int row, int pos) override {
        const auto wrap = [](int v, int n) { return (v % n + n) % n; };
        const int delta = pos - cur[row];
        if (row == count_ - 2) selected_ = wrap(selected_ + delta, play::kPadSrcCount);
        else pending_[selected_] = wrap(pending_[selected_] + delta,
            saved_[selected_] == play::kPadChoiceCount ? play::kPadChoiceCount + 1 : play::kPadChoiceCount);
        cur[row] = orig[row] = 127; // selector positions are navigation, not settings
    }
    bool hidden_changes() const override {
        return !std::equal(std::begin(pending_), std::end(pending_), std::begin(saved_));
    }
    void commit(Apply& a) override {
        StaticTab::commit(a);
        for (int i = 0; i < play::kPadSrcCount; ++i)
            if (pending_[i] != saved_[i] && pending_[i] < play::kPadChoiceCount) {
                a.s.pad.remap[i] = play::pad_choice_value(pending_[i]);
                a.changed = true;
            }
    }
    void accept() override {
        TabModel::accept();
        std::copy(std::begin(pending_), std::end(pending_), std::begin(saved_));
    }
    void revert() override {
        StaticTab::revert();
        std::copy(std::begin(saved_), std::end(saved_), std::begin(pending_));
    }
private:
    int selected_ = 0;
    int pending_[play::kPadSrcCount] = {}, saved_[play::kPadSrcCount] = {};
};
} // namespace
TabModel& controller_tab() { static ControllerTab tab; return tab; }
} // namespace nxrt::games::pkp::options_detail
