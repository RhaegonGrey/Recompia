// SPDX-License-Identifier: GPL-3.0-or-later
// The machine the runtime runs on, where it changes a default (src/host_device.cpp).
#pragma once

namespace nxrt {

// A gaming handheld played with its own controls: a Steam Deck (LCD or OLED), a Lenovo Legion Go, Go S or Go 2, an ASUS ROG Ally,
// Ally X or ROG Xbox Ally, or an MSI Claw A8, under SteamOS/Proton or Windows. Detected from Steam's SteamDeck=1 in the environment or
// the board (the list in src/host_device.cpp, matched on the SMBIOS product name or family), read from the registry's BIOS key (Windows,
// and Wine fills it from SMBIOS) or Wine's Z:\sys\class\dmi.
// [host] handheld = auto (default) | on | off overrides it. What it changes: [display] render_scale auto renders at the window's
// size instead of at least 1920x1080 (a handheld's GPU), and [prompts] style auto keeps controller prompts when the trackpad,
// touchscreen or on-screen keyboard send mouse or keyboard input.
bool host_is_handheld();

// A Steam Deck (LCD or OLED) only: Steam's SteamDeck=1 or the board Jupiter / Galileo (same sources as above; [host] handheld does not
// change it). For defaults tied to the Deck's fixed-refresh panel, such as [display] fps_snap auto.
bool host_is_steam_deck();

// The host kernel is Linux: true for the native Linux build and for a Windows build running under Wine on Linux (Proton, SteamOS), false
// on Windows and macOS. This is the question a default's reason is the kernel, its scheduler or a Mesa driver, because those apply under
// Proton and natively alike: RADV revalidating every imported userptr BO of a submission, amdgpu faulting the pages of one in, mprotect
// invalidating them, a listener's TIME_WAIT, /proc, the SteamOS session. Nothing asks whether the process is Wine any more: what Wine
// does differently is asked as a capability (is winebrowser there, did SDL report the pad) where it is needed.
bool host_is_linux();
// Logs read-only Linux host facts for bug reports; no identifying fields.
void host_log_facts();
// Steam's on-screen keyboard for text the game asks for (the software keyboard applet). Wanted under Wine (Proton) on a Steam Deck
// (host_is_steam_deck), on SteamOS (SteamOS=1) and in Steam's gamepad UI (SteamGamepadUI=1); [applet] steam_keyboard = auto (default)
// | on | off overrides it. Not on Windows by default: a steam:// URL there would start the Steam client if it is not running.
bool host_steam_keyboard_wanted();
// Shows Steam's keyboard (steam://open/keyboard), keeping the text field at window pixels (x, y, w, h) uncovered; Steam types into the
// window like a real keyboard. Close hides it again (steam://close/keyboard). Both return at once: a worker hands the URLs to Steam in
// order (under Proton through the prefix's steam.exe, Proton's helper that forwards steam:// to the native client).
void host_steam_keyboard_open(int x, int y, int w, int h);
void host_steam_keyboard_close();

}  // namespace nxrt
