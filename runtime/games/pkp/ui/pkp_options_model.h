// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options: what a tab is to the screen (pkp_options.cpp). A tab is a TabModel: its rows (label, description, widget, value texts),
// their values while the screen is open (cur / orig, one per slot) and what happens when a row moves, the screen commits or it discards.
// pkp_options.cpp keeps only the slot state machine and the game's hooks and asks the model. StaticTab serves the constexpr Row tables
// (pkp_options_internal.h); the Time, Events, Mods and Items tabs are their own models.
#pragma once
#include <cstddef>
#include <cstdint>

#include "ui/pkp_options_internal.h"

namespace nxrt::games::pkp::options_detail {

// What happens when a row's value moves (the game's store, 0x100b920).
enum class Use : uint8_t {
    Value,      // the game stores it; it takes effect when the screen commits
    Live,       // the same, and preview() shows it at once
    Capture,    // moving it starts a key capture for capture() (pkp_options_capture.h); the client saves the key at once
    Press,      // a button: press() acts at once and sets the tab's values; the game's store is skipped
    ReadOnly,   // shows a value; the game's store is skipped, nothing changes
    Rows,       // moving it changes the tab's own rows (pick()): the screen rebuilds the list
};

// The game's widget for a row: a choice of 2..4 named values, or a slider over lo..hi (0..255; the record holds bytes).
struct Shape {
    uint8_t choices = 0;   // 2..4: a choice; 0: a slider
    int lo = 0, hi = 0;    // a choice: 0..choices-1
    Use use = Use::Value;
};

// Built by the screen when it commits: the play-mode Settings with every tab's changes, applied once at the end.
struct Apply {
    S s;
    bool changed = false;   // a row of play-mode Settings changed: play::apply_settings(s)
};

class TabModel {
public:
    int cur[kSlots] = {}, orig[kSlots] = {};   // the positions: cur as the screen shows them, orig as in force (a slot per row)

    virtual ~TabModel() = default;
    virtual int count() const = 0;   // rows shown, at most kDirtySlot
    virtual Shape shape(int i) const = 0;
    virtual const char* label(int i) const = 0;
    virtual const char* desc(int i) const = 0;
    virtual bool restart(int i) const = 0;   // " Applies after a restart." after the description
    // A slider's text at a position, or a choice's value name (pos 0..choices-1). May write to out, or return a string the tab owns
    // (valid until its rows change).
    virtual const char* value_text(int i, int pos, char* out, size_t n) const = 0;
    virtual int def(int i) const = 0;   // the record's default position
    virtual CaptureClient* capture(int) const { return nullptr; }   // Use::Capture: who takes the key

    virtual void open() = 0;                                      // the screen opened: cur = orig = the values in force
    virtual void commit(Apply& a) = 0;                            // the screen commits: the changed rows take effect
    virtual void accept() { for (int i = 0; i < kSlots; ++i) orig[i] = cur[i]; }   // after every tab's commit
    virtual void revert() { for (int i = 0; i < kSlots; ++i) cur[i] = orig[i]; }   // the screen discards
    virtual void preview(int, int) {}                             // Use::Live: row i moved to pos (cur holds it already)
    virtual void press(int, int) {}                               // Use::Press: row i was pressed (sets cur and orig itself)
    virtual void pick(int, int) {}                                // Use::Rows: row i moved to pos; the rows may change
    // Changes the vector does not show while this tab is the one on screen (rows of the tab that are not on screen now).
    virtual bool hidden_changes() const { return false; }

    bool changed() const {
        for (int i = 0; i < count(); ++i)
            if (cur[i] != orig[i]) return true;
        return hidden_changes();
    }
};

// The constexpr Row tables (pkp_options_rows.cpp): a row of play-mode Settings (Row::get / Row::set) or of a [pkp] key (Row::key).
class StaticTab : public TabModel {
public:
    explicit StaticTab(RowList r, void (*on_open)() = nullptr) : rows_(r.rows), count_(r.count), on_open_(on_open) {}

    int count() const override { return count_; }
    Shape shape(int i) const override;
    const char* label(int i) const override { return rows_[i].label; }
    const char* desc(int i) const override { return rows_[i].desc; }
    bool restart(int i) const override { return rows_[i].restart; }
    const char* value_text(int i, int pos, char* out, size_t n) const override;
    int def(int i) const override;
    CaptureClient* capture(int i) const override { return rows_[i].capture; }
    void open() override;
    void commit(Apply& a) override;
    void revert() override;
    void preview(int i, int pos) override;

protected:
    const Row* rows_;
    int count_;
    void (*on_open_)();
};

// ---- the tabs that are their own models ----
TabModel& controller_tab(); // pkp_options_controller.cpp: pending controller button mappings
TabModel& time_tab();     // pkp_options_time.cpp: the game clock, applied when the screen commits
TabModel& events_tab();   // pkp_options_time.cpp: one button per event; pressing one moves the clock and the Time tab's rows at once
TabModel& mods_tab();     // pkp_options_mods.cpp: the Modtopia mods, one at a time: switch, state and settings
TabModel& items_tab();    // pkp_options_items.cpp: a stack of the bag and its new count, written to the bag when the screen commits

}  // namespace nxrt::games::pkp::options_detail
