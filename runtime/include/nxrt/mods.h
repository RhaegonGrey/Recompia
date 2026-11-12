// SPDX-License-Identifier: GPL-3.0-or-later
// The mods host: runs Modtopia mods (third_party/modtopia, MIT; its docs/HOST.md is the contract) on the game. Game-independent: the game
// supplies its folders and the host's name through game::Info::mods (nxrt/game.h) and calls the entry points below at the right moments.
//
//   start()            once, after the game's modules are mapped and linked and before its first instruction: writes the embedded mappings,
//                      finds the mods, lays the data mods over the RomFS (services fsapi::set_romfs_overlays, under [fs] romfs_mod),
//                      registers the code mods' hook points at the sites the game build has (game::ModHost::hooks_built), starts the code
//                      mods, applies the mods' patches to the game's files and checks the data mods. Without a mods folder it does nothing
//                      but log one line, and the game runs exactly as without the host. Code the player has not allowed in the launcher
//                      (MT_MOD_NEW_CODE on a mod that is on) never runs: then no code mod runs this time and code mods' files wait too;
//                      the player's saved switches stay; each such mod's MtModInfo.message says why, and a message over the picture
//                      (nxrt/notices.h) names each new one. With [mods] off (env NXRT_MODS_OFF) it loads nothing and touches nothing of the
//                      mods: a start without mods, logged and noticed over the picture.
//   frame()            once a game frame, on the game's thread (the core's FrameStart, AreaEntered, SaveLoaded).
//   SaveWritten        posted by the host itself after each save data commit (services fsapi::set_save_commit_listener).
//   gameplay_mods()    for Link Play: the mods that change gameplay, "id@version", sorted. A game appends them after its own features in
//                      the session's mods fingerprint (services ldnapi::set_coop_mods), so two hosts refuse the same mismatches.
//   loader()           the core itself, for a mod list and mod settings (modtopia/modtopia_host.h: mt_loader_count, mt_setting_*, ...).
//   started_without()  true when this start runs without mods ([mods] off), so a mod list can say why it is empty.
//   locked(id)         the launcher keeps this mod on ([mods] locked, ids): a mod list must not switch it off.
//   setting_locked(id, key)  the launcher keeps this setting of a mod on ([mods] locked_settings, `id:key`): a mod list must not change it.
//                      Both lists are the launcher's (it writes them into the ini); this host names no mod. Read at start.
#pragma once
#include <string>
#include <vector>

struct MtLoader;   // modtopia/modtopia_host.h

namespace nxrt::mods {

struct StartOptions {
    const char* host_version = "";   // the build's version, shown to mods after game::ModHost::host ("Recompia 0.12.0")
};

void start(const StartOptions& opt);
void frame();
std::vector<std::string> gameplay_mods();   // empty when no gameplay mod is on (or before start)
MtLoader* loader();                         // nullptr when no mods folder was found, or this start runs without mods (or before start)
bool started_without();                     // this start runs without mods ([mods] off)
bool locked(const char* id);
bool setting_locked(const char* id, const char* key);

}  // namespace nxrt::mods
