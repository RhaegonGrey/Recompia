// SPDX-License-Identifier: GPL-3.0-or-later
// Cross-service and host-facing entry points of the service library (S3c): what one service asks of another, and what the PC
// layers (window focus, settings UI) call to change console state. Implemented in the .cpp of the service named in each comment.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace nxrt::services {

// ---- fs (fs.cpp): save data backend used by am (EnsureSaveData ...) and the fsp-srv commands ---------------------------------------
// A user id is the 128-bit nn::account::Uid as two u64 halves in memory order: the first u64 (uid[0]) is "hi", the second is "lo" (profile().user_uid_hi / _lo).
// Results are Horizon result codes (0 = success; the module-2 fs results otherwise).
namespace fsapi {
// Creates the account save data of the title for `uid` when it does not exist (size and journal from the profile). *created tells which.
uint32_t ensure_account_save(uint64_t uid_hi, uint64_t uid_lo, bool* created);
// Size and journal size of the account save data (the values nn::fs::GetSaveDataSize reports); the fs "path not found" result when absent.
uint32_t account_save_size(uint64_t uid_hi, uint64_t uid_lo, int64_t* size, int64_t* journal);
// Save commits that failed (fs_volume.cpp: a write failed, or the commit itself could not be written) since the start, and the Win32 error
// of the last one. The last good save is kept. The overlay shows a notice when the count grows. Any thread.
struct SaveFailures {
    uint32_t count = 0;
    uint32_t host_error = 0;
};
SaveFailures save_failures();
const char* save_failure_reason(uint32_t host_error);   // "the disk is full", ... (for the notice)

// What a mod loader asks of the file system (the mods host, nxrt/mods.h). Any thread.
// Folders laid over the game's RomFS below [fs] romfs_mod, index 0 highest: the same path in two layers is served from the earlier one,
// folders merge. They apply to the RomFS the game opens; once it has opened it they are fixed (false, and nothing changes). Folders that
// cannot be layered over the game's files are left out with an error at the first open, and the game starts without them.
bool set_romfs_overlays(const std::vector<std::string>& roots);
// The game's own RomFS files ([fs] romfs over [fs] romfs_base, no mod folder), by RomFS path ("a/b.bin", relative): a file's size, -1
// when the game has none; up to n bytes of it from offset into buf, how many were read (fewer only at its end), -1 on an error. The file
// table is built on first use and kept until romfs_game_release().
int64_t romfs_game_file_size(const std::string& path);
int64_t romfs_game_read(const std::string& path, uint64_t offset, void* buf, uint64_t n);
void romfs_game_release();
// fn runs after every successful Commit of a save data file system, on the committing thread (the game's). One listener; null: none.
void set_save_commit_listener(void (*fn)());
}  // namespace fsapi

// ---- am (am.cpp): console state changes from the host --------------------------------------------------------------------------------
// Each call queues the matching AppletMessage for the guest (it sees them through ICommonStateGetter::ReceiveMessage, with the message
// event signalled) and updates the state the getters report.
namespace amctl {
void set_docked(bool docked);          // OperationModeChanged (30) + PerformanceModeChanged (31); GetOperationMode/GetPerformanceMode follow
void set_focus(bool in_focus);         // FocusStateChanged (15); GetCurrentFocusState: 1 = InFocus, 2 = OutOfFocus
bool docked();
bool in_focus();
}  // namespace amctl

// ---- the software keyboard library applet (am.cpp, swkbd_prompt.cpp) ---------------------------------------------------------------
// Where the keyboard's text comes from, in order: [applet] swkbd_text when set (test runs: NXRT_APPLET_SWKBD_TEXT); else the on-screen
// keyboard when enabled (drawn by the overlay inside the game window; the play layer enables it when there is an overlay); else the initial
// text the game gave; else "Player".
namespace swkbdui {
struct Request {
    std::string title;        // the window title (the game's name)
    std::string header, sub, guide, ok_text, initial;   // UTF-8, from the keyboard config and work buffer
    uint32_t max_len = 0;     // characters, 0 = no limit
    uint32_t min_len = 0;
    bool password = false;
};
void enable_prompt(bool on);
bool prompt_enabled();
// Posts the question and waits until the keyboard answers it. True with *text set on OK, false on cancel. Any thread but the one drawing.
bool ask(const Request& r, std::string* text);
// For the keyboard: the open question's id (0 = none), with *r set when there is one; answer() closes it (a stale id is ignored).
uint64_t pending(Request* r);
void answer(uint64_t id, bool ok, const std::string& text);
}  // namespace swkbdui

// ---- time (time.cpp) ------------------------------------------------------------------------------------------------------------
// The game clock moved at run time ([time] clock_offset is the same offset at start-up).
namespace timetravel {
int64_t offset();                                                        // seconds the game clock is from the host clock (0 = real time)
int64_t real_now();                                                      // the host's UTC time, unix seconds
bool local_calendar(int64_t posix, int v[6]);                            // year, month, day, hour, minute, second under the device zone
bool posix_of_local(int y, int mo, int d, int h, int mi, int64_t* out);  // the inverse; false for a time that does not exist
void set_offset(int64_t seconds);                                        // moves the game clock and tells the game the clocks changed
}  // namespace timetravel

// ---- vi (vi_nv.cpp)----------------------------------------------------------------------------------------------------------------
namespace viapi {
// A new readable handle of the display's 60 Hz vsync event (starts the vsync thread on first use); 0 when no handle is left. For the
// Switch 2 nn::vi functions that answer on the host (core/switch2.cpp).
uint32_t display_vsync_handle();
}

// ---- acc (acc.cpp) ----------------------------------------------------------------------------------------------------------------
namespace accapi {
void user_uid(uint64_t* hi, uint64_t* lo);   // the one local user (profile())
}

// ---- ldn (ldn.cpp) ----------------------------------------------------------------------------------------------------------------
// Session variant: a gameplay change every player of a Link Play session must share (a game module's opt-in mod). 0 = none (the game as
// shipped), 1..7 = the game module's own meaning. It rides in the room's local communication version, which the relay already compares on
// JOIN (bits 12..14: wire = version | variant << 12; the game's own version stays in bits 0..11 and is all the game ever sees), so peers
// with another variant, or an older build without variants, are refused like a version mismatch (the relay protocol is unchanged).
// `what` names the game module's setting for the log ("Interact everywhere"), also with variant 0, so a refusal can say what the host has
// on. Set it before the game creates or joins a network; any thread.
namespace ldnapi {
constexpr int kVariantShift = 12;
constexpr uint16_t kVariantMask = 0x7000;
void set_session_variant(uint8_t variant, const char* what);
uint8_t session_variant();
// Co-op mods: more opt-in mods a game module has on, by name, for [net] protocol = native only. The co-op module's mods fingerprint, which
// peers compare before a join, is the session variant's name (while a variant is set) followed by these names in this order; nothing
// (0) when both are empty. A name must be byte for byte what the other builds use. Empty: none. Set it like the variant; any thread.
void set_coop_mods(const std::vector<std::string>& names);
std::vector<std::string> coop_mods();
// Whether ldn:u speaks [net] protocol = native (the default).
bool native_protocol();
}  // namespace ldnapi

}  // namespace nxrt::services
