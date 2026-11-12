// SPDX-License-Identifier: GPL-3.0-or-later
// In-game Options, the Time and Events tabs (pkp_options_model.h); the pure part (event file, calendar) is pkp_time.cpp.
// Time: the game clock moved to a date and time (nxrt::services::timetravel, time.cpp). The date rows show the game's clock; "Time Travel"
// On sets the game to them when the screen commits, Off puts the clock back on the real time. Nothing reaches Settings or the save file.
// Events: one button row per line of [pkp] events_file (pokopia_events.tsv, read every time the screen opens); pressing one sets the game
// clock to noon on the day the event starts, at once, fills the Time tab with it and marks that row "Chosen" (only one at a time).
#include "ui/pkp_options_model.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/pkp_guest.h"
#include "core/pkp_settings.h"
#include "nxrt/log.h"
#include "nxrt/services_api.h"
#include "play_game.h"
#include "pkp_time.h"

namespace nxrt::games::pkp::options_detail {
namespace {

constexpr int kEventHour = 12;                 // the hour of the day the clock is set to

struct Shown { int v[6]; };                    // year, month, day, hour, minute, second of the game's clock now
Shown shown_clock() {
    Shown s{};
    namespace tt = services::timetravel;
    if (!tt::local_calendar(tt::real_now() + tt::offset(), s.v)) s.v[0] = kFirstYear, s.v[1] = s.v[2] = 1;
    return s;
}

// clang-format off
#define TIME_ROW(k, off, hi) [](const S&) { return clamp_pos(shown_clock().v[k] - (off), (hi) + 1); }, [](S&, int) {}

constexpr const char* kClockWords[3] = {"auto", "12h", "24h"};
constexpr const char* kClockNames[4] = {"Game", "12 hour", "24 hour"};
int clock_format_index(const std::string& f) { return f == "12h" ? 1 : f == "24h" ? 2 : 0; }

const Row kTime[] = {
    choice("Time Travel", "Off: the game follows the real date and time. On: the game is set to the date and time below when you leave this screen.",
           kOffOn, 2, [](const S&) { return services::timetravel::offset() != 0 ? 1 : 0; }, [](S&, int) {}),
    number("Year", "The year the game clock is set to. Time Travel must be On.", 0, kYearCount - 1, "%.0f", kFirstYear, 1, TIME_ROW(0, kFirstYear, kYearCount - 1)),
    number("Month", "The month the game clock is set to.", 0, 11, "%.0f", 1, 1, TIME_ROW(1, 1, 11)),
    number("Day", "The day of the month (a day past the end of the month becomes its last day).", 0, 30, "%.0f", 1, 1, TIME_ROW(2, 1, 30)),
    number("Hour", "The hour of the day, 0 to 23.", 0, 23, "%.0f", 0, 1, TIME_ROW(3, 0, 23)),
    number("Minute", "The minute.", 0, 59, "%.0f", 0, 1, TIME_ROW(4, 0, 59)),
    choice("Clock Notation", "How the game shows the time of day. Game: its language decides (English 12 hours with AM/PM, German 24 hours). A screen that is already open shows the new notation after you close it and open it again.", kClockNames, 3,
           [](const S& s) { return clock_format_index(s.clock_format); }, [](S& s, int v) { s.clock_format = kClockWords[std::clamp(v, 0, 2)]; }),
};

std::vector<TimeEvent> g_events;               // what the Events tab shows, in row order (the labels point into it)
std::string g_event_dates[kMaxEvents];         // each row's description
std::string g_none_desc;
Row g_event_rows[kMaxEvents];                  // the Events tab's rows, filled by load_events

// An event row is a two-state button: "Chosen" while the game clock is on the day the event starts (Time Travel on), else "Choose".
template <int I> int event_chosen(const S&) {
    namespace tt = services::timetravel;
    if (I >= static_cast<int>(g_events.size()) || tt::offset() == 0) return 0;
    const Shown c = shown_clock();
    return c.v[0] * 10000 + c.v[1] * 100 + c.v[2] == g_events[I].start ? 1 : 0;
}
template <int... I> constexpr std::array<int (*)(const S&), sizeof...(I)> event_getters(std::integer_sequence<int, I...>) {
    return {&event_chosen<I>...};
}
constexpr auto kEventChosen = event_getters(std::make_integer_sequence<int, kMaxEvents>());
constexpr const char* kChoose[4] = {"Choose", "Chosen"};

// clang-format on

int real_today() {
    int v[6] = {kFirstYear, 1, 1, 0, 0, 0};
    services::timetravel::local_calendar(services::timetravel::real_now(), v);
    return v[0] * 10000 + v[1] * 100 + v[2];
}

// Reads the event file again (the launcher or a script may have changed it) and fills the Events tab's rows.
int load_events() {
    std::stringstream text;
    std::ifstream file(settings::get_str(settings::Key::EventsFile));
    if (file) text << file.rdbuf();
    const int today = real_today();
    g_events = parse_events(text.str(), today, kMaxEvents);
    for (size_t i = 0; i < g_events.size(); ++i) {
        g_event_dates[i] = event_dates(g_events[i], today) + ". Press to set the game clock to this day, at noon.";
        g_event_rows[i] = Row{g_events[i].name.c_str(), g_event_dates[i].c_str(), {kChoose[0], kChoose[1]}, 2, 0, 0, nullptr, 0, 0, nullptr, nullptr, nullptr,
                              nullptr, kEventChosen[i], [](S&, int) {}, false, false};
    }
    if (g_events.empty()) {
        g_none_desc = std::string("No events in ") + settings::get_str(settings::Key::EventsFile) +
                      ". The file has one event per line: start date, end date and name, separated by tabs (2026-11-16 2026-12-01 Name).";
        g_event_rows[0] = Row{"No events found", g_none_desc.c_str(), {kChoose[0], kChoose[1]}, 2, 0, 0, nullptr, 0, 0, nullptr, nullptr, nullptr,
                              nullptr, [](const S&) { return 0; }, [](S&, int) {}, false, false};
    }
    NXLOG_I(kTag, "options: %zu events from %s", g_events.size(), settings::get_str(settings::Key::EventsFile).c_str());
    return g_events.empty() ? 1 : static_cast<int>(g_events.size());
}

// The Time tab's rows, as the game clock: Travel Off puts the real time back, On sets the date and time of the rows.
void apply_time_tab(const int (&c)[kSlots]) {
    namespace tt = services::timetravel;
    if (c[0] == 0) {
        tt::set_offset(0);
        return;
    }
    const int year = year_of(c[1]), month = 1 + c[2];
    int64_t at = 0;
    if (!tt::posix_of_local(year, month, std::min(1 + c[3], days_in_month(year, month)), c[4], c[5], &at)) {
        NXLOG_W(kTag, "options: %04d-%02d-%02d %02d:%02d does not exist in this time zone: the clock stays as it is", year, month, 1 + c[3], c[4], c[5]);
        return;
    }
    tt::set_offset(at - tt::real_now());
}

// An Events row was pressed: the game clock goes to noon on the event's first day now, and the Time tab shows it.
void go_to_event(int row, int (&time_cur)[kSlots], int (&time_orig)[kSlots]) {
    namespace tt = services::timetravel;
    if (row < 0 || row >= static_cast<int>(g_events.size())) return;
    const int start = g_events[row].start, year = start / 10000, month = start / 100 % 100, day = start % 100;
    int64_t at = 0;
    if (!tt::posix_of_local(year, month, day, kEventHour, 0, &at)) {
        NXLOG_W(kTag, "options: %s starts on a date that does not exist in this time zone", g_events[row].name.c_str());
        return;
    }
    tt::set_offset(at - tt::real_now());
    const int rows[6] = {1, clamp_pos(year - kFirstYear, kYearCount), month - 1, day - 1, kEventHour, 0};
    for (int i = 0; i < 6; ++i) time_cur[i] = time_orig[i] = rows[i];
    NXLOG_I(kTag, "options: game clock set to %04d-%02d-%02d %02d:00 (%s)", year, month, day, kEventHour, g_events[row].name.c_str());
}

constexpr int kClockRows = 6;   // Time Travel, Year .. Minute: the game clock; the rows after them are play-mode Settings

class TimeTab final : public StaticTab {
public:
    TimeTab() : StaticTab(rows_of(kTime)) {}
    void commit(Apply& a) override {
        if (std::memcmp(cur, orig, kClockRows * sizeof(int)) != 0) apply_time_tab(cur);
        StaticTab::commit(a);
    }
};

// Each row is a two-state button (kChoose); pressing one goes to the event now and keeps the row where it was (nothing to save).
class EventsTab final : public StaticTab {
public:
    explicit EventsTab(TabModel& time) : StaticTab(RowList{g_event_rows, 1}), time_(time) {}
    void open() override {
        count_ = load_events();   // the count follows the event file
        StaticTab::open();
    }
    Shape shape(int i) const override { return Shape{rows_[i].choices, 0, rows_[i].choices - 1, Use::Press}; }
    void press(int row, int) override {
        go_to_event(row, time_.cur, time_.orig);
        for (int i = 0; i < kSlots; ++i) cur[i] = orig[i] = i == row ? 1 : 0;   // exactly one event is chosen
    }

private:
    TabModel& time_;
};

TimeTab g_time;
EventsTab g_events_tab{g_time};

}  // namespace

TabModel& time_tab() { return g_time; }
TabModel& events_tab() { return g_events_tab; }

// The game's time-of-day text. [tweaks] clock_format: 12h or 24h write the text here; auto leaves the language's own layout to the game.
int hook_clock_text(::Ctx* c, uint64_t, void*) {
    const std::string& f = play::current_settings().clock_format;
    const int format = clock_format_index(f);
    if (format == 0) return 0;
    const int hour = static_cast<int>(c->x[0] & 0xffffffff) % 24, minute = static_cast<int>(c->x[1] & 0xffffffff) % 60;
    const uint64_t out = c->x[2];
    char text[40];
    if (format == 2) std::snprintf(text, sizeof text, "%02d:%02d", hour, minute);
    else std::snprintf(text, sizeof text, "%d:%02d<nbs2>%s", hour % 12 == 0 ? 12 : hour % 12, minute, hour < 12 ? "AM" : "PM");
    for (size_t i = 0; i <= std::strlen(text); ++i) guest::wr<uint8_t>(out + i, static_cast<uint8_t>(text[i]));
    return 1;
}

}  // namespace nxrt::games::pkp::options_detail
