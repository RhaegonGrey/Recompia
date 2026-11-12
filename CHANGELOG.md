# Changelog

## Unreleased

### Your own launcher background
- The launcher can use a picture from your PC as its background (Testing channel)
  - Before: the launcher always showed its built-in pictures
  - Now: open **Layout** in the sidebar, then **Appearance > Choose picture...**; pick a PNG, JPEG or WebP, set **Dim** so the text stays easy to read, and **Reset to default** brings the built-in picture back; the picture stays on this PC and is never uploaded
  - Still not: no animated pictures, and no different picture for each page

**Update:** press **Update**, then **Set up again** once. It rebuilds the game code for two new hooks, Story co-op and the shoulder camera. Your saves and shaders are kept; no shader is translated again. A later build that adds no new hooks should not ask again.

### Testers on AMD handhelds, please read
This Testing build adds logging and a few device checks, and changes no frame-rate settings. We need your logs to learn what helps on the Steam Deck, ROG Ally, Legion Go and similar devices. Play for about twenty minutes the way you normally do, then open **Settings > Testing** and press **Copy handheld report**: it copies one short block with your device, memory, graphics driver, frame summaries and any problems, with user names, folders and addresses removed. Paste it into a bug report and tell us the device, the system (SteamOS, Bazzite and so on), the power mode, your frame cap and what you were doing. If the picture drops to about 10 fps and stays there, or the game freezes, say so and restart the device. This is a Testing build: some rough edges are expected, and your reports are how Stable gets better.

### Why textures are not prepared in the background
- `pkp.log` now says why the background texture preparation is rarely used
  - Before: the `nvn.texprep` line only said "used 0, no job N"
  - Now: the same line also counts the reasons a texture upload got no job: too small, too large, not block-compressed, GPU off, never considered, idle, stale, or a transcode still running
  - Still not: nothing else changes; the texture preparation behaves exactly as before (the 480 unknown-proc lines at startup stay as they are for now)

### Memory limit under Proton
- The settings reference now says the memory limit defaults to 16 GB for Pokopia (it said 8 GB, which is the default for other games)
- The game's memory limit ([memory] limit_gb) now works under Proton
  - Before: under Proton the reading the limit compares (private commit) can be 0, so the limit never triggered even when the game used many gigabytes; `pkp.log` showed `footprint 0.00 GB` next to a real pagefile usage
  - Now: when the private commit reads 0, the limit compares the pagefile usage (resident anonymous memory plus swap), so `footprint` shows the real figure and the limit can trigger before the device runs out of memory; the first time it happens `pkp.log` says so
  - Still not: the limit is 16 GB by default, so on a normal run nothing changes except the log; if the game is stopped at the limit, a report is written and the numbers in it are the same as before

### Regressions and known problems
- The Lenovo Legion Go, Go 2, ROG Xbox Ally and MSI Claw A8 now start in handheld mode (picture size, the game's lighter handheld graphics settings, controller prompts, short waits under Proton). This has not been confirmed on those devices yet; if it looks worse than before, set `[am] operation_mode = docked` or `[host] handheld = off` in `pkp.ini`
- Everyone needs **Set up again** once for this build (two new game hooks). Tell us how long it took and on which device
- The Steam Controller (2026) is still not detected under Proton; use another pad there

### What to test
- On any AMD handheld (Steam Deck, ROG Ally, Legion Go, Legion Go S, Xbox Ally, Claw A8), under Proton or Windows: play 10 minutes, then send the lines of `pkp.log` that contain `host: handheld`, `pool_copy`, `footprint` and `gamepad`. Say which BIOS GPU-memory setting you use if you changed it. They show which handheld rule matched, why pool copy is on or off with the graphics memory size, and the two memory readings
- On a Legion Go, Go 2, ROG Xbox Ally or MSI Claw A8: check that `pkp.log` says `host: handheld: yes`, and tell us whether the picture size, the controller prompts and the frame rate feel right compared with the previous build
- If the frame rate falls partway through a session and stays low: send the `pkp.log` lines that contain `frames have been slow` and the `hitch:` lines around them (each now ends with the share of time spent waiting), and say how long into the session it began
- Launcher background (Layout > Appearance > Choose picture...): pick a PNG, JPEG or WebP, move the Dim slider, press Reset to default; tell us if the text is still easy to read
- Shoulder camera (Settings > Gameplay: Shoulder camera: side and height): tell us what you set and what looked wrong
- If Set up stops on a Steam Deck or another Linux handheld: send `shader_cache/stage/harness_error.txt` and `setup.log`
- If you own a Steam Controller (2026): follow the 15-minute check in `docs/steam-controller-check.md` (which grip is which, whether the small View and Menu buttons are the right way round, the trackpad clicks) and send the table with the exported logs

### Controller log
- `pkp.log` now lists what a connected controller offers: trackpads (and their fingers), rumble, lights, battery, extra buttons (grips, trackpad clicks, touch sensors) and its USB ids; the motion sensors are on the line before it. It is a note for bug reports; nothing about how controllers behave changes

### Memory limit
- `pkp.log` now shows both memory readings (private commit and pagefile usage) in its `footprint` line and in the 75% warning, and says once when the private-commit reading is 0
  - Before: only the footprint was shown, so on Linux (Proton) a log reading `footprint 0.00 GB` could not tell whether the game used no memory or the reading was blind
  - Now: the log names both readings; the limit itself is checked exactly as before
  - Still not: nothing else changes here; the limit itself under Proton is covered by "Memory limit under Proton" above

### Launcher in Steam game mode
- On a Steam Deck, ROG Ally or Legion Go in game mode, the launcher now fills the screen and uses finger-sized controls
  - Before: on a 1080p handheld screen the launcher opened as a small floating window, and its buttons, switches and text were small
  - Now: when Steam game mode is detected, the window starts maximized, every button, menu item, switch and text field is at least 44 pixels tall, and the text is a little larger
  - Still not: this has not been tried on a real handheld yet, and the launcher does not yet have a larger-text setting of its own
- **Joy-Con 2 mouse mode** is back: press the middle mouse button in play to point and click, and placing and breaking now land on the block under the pointer (left click is the Joy-Con's R, right click its ZR, as in Pikantine). Press it again to go back to camera aim. The **Mouse Mode Key** row is back in **Settings > Mouse**. Update only: no shader rebuild, no Set up again
- New **Show on Discord while playing** in the launcher's Settings: shows "Playing Recompia" and how long you have played on your Discord profile while the game runs. Off by default; Discord must be open. Update only: no shader rebuild, no Set up again

### Launcher layout
- Added a **Layout** button in the launcher's sidebar, beside Colours and Hide sidebar. It opens a panel where you choose how the launcher is arranged: the sidebar on the left, right, top or bottom, or icons only; the order of the sidebar items, and which of them to hide (Home and Settings always stay); comfortable or compact spacing; and whether the news cards and the chips under the Play button (profile, Link Play and Discord) show on Home. Changes show at once, **Reset to default** puts everything back, and your choice is kept on your PC
  - Before: the launcher had one fixed arrangement
  - Now: the arrangement is yours, and the pad, keyboard and touch screen all work in every position. Notices about a problem, such as a crash or a failed update, are never hidden
  - Still not: you cannot drag parts around freely yet, and the sections of Home other than the news cards and those chips cannot be moved or hidden
- New **Show on Discord while playing** in the launcher's Settings: shows "Playing Recompia" and how long you have played on your Discord profile while the game runs. Off by default; Discord must be open. Update only: no shader rebuild, no Set up again

### Bug reports from Linux handhelds
- `pkp.log` now starts with a `host facts:` line on Linux (Steam Deck, Legion Go, ROG Ally and other handhelds under Proton): kernel, board and BIOS version, CPU, power governor, the graphics memory split (VRAM, GTT, TTM limit), RAM, swap, zram and whether it runs in Gamescope
  - Before: reports did not say how the memory was split or how the power was set, so the performance questions could not be answered from the log
  - Now: the line holds those facts, read only; it never includes serial numbers, your user name, your computer's name or folder paths, and a value the device does not have is left out
  - Still not: it changes nothing about how the game runs; it only helps reports. Not shown on Windows

### Graphics driver resets
- `pkp.log` now notices a slowdown that starts partway through a run, not only at the start
  - Before: it only looked at the first minutes of a run and at one kind of wait, so a handheld that played the title screen fine and slowed to about 10 fps once the world loaded was never noticed
  - Now: when three summary windows in a row (about 30 seconds) run under 20 fps with the game thread mostly waiting, `pkp.log` says when it began and how much of the time was waiting; the 10-second `hitch:` line also shows the share of waiting in each window, which helps tune the check from real logs
  - Still not: it is a note for bug reports, not a fix. After a graphics driver reset on a Linux handheld only a reboot of the device clears it; the README's common problems say so too

### Screens narrower than 16:9
- Added **4:3 Fix**, a very experimental option in Settings > Testing (Testing channel only, off by default) for 4:3, 16:10 and 3:2 screens: full-screen backgrounds, fades and dimming fill the whole picture instead of stopping at a 16:9 strip in the middle. Menus, text and buttons stay where they were. If you have a 4:3 or 16:10 screen, please try it and send `pkp.log` with what looked wrong. It needs a restart of the game to take effect
- `pkp.log` also states how many megapixels a frame costs at the picture's shape

### Link Play invites
- Added **Copy invite** and **Paste invite** to the launcher's Link Play page: the host copies one message (address, room code and mode) and the friends paste it, instead of typing both. A Tailscale address is recognised and set up as internet play, and the host's page now lists the PC's Tailscale address even without a relay. Typing a Tailscale address into Same network now says what to do instead of just refusing it
- Added **Test connection**, **Check my setup** and **Friends on my Tailscale** to the Link Play page: they say in plain words whether an address answers, what may block friends from joining (firewall, a Public network, client isolation), and which of your Tailscale devices are hosting, with a Join button. They only look and never change anything

### Pool copy diagnostics
- `pkp.log` now states why pool copy is on or off: whether the host is Wine, the graphics card type and its largest device-local memory heap in MiB, and the 8 GiB limit

### More handhelds recognised
- The Lenovo Legion Go, Go 2, the ASUS ROG Xbox Ally and the MSI Claw A8 (the ROG Ally X was already recognised) are now treated as handhelds, like the Steam Deck, Legion Go S and ROG Ally
  - Before: only the Deck, Legion Go S and ROG Ally were recognised; on the others the game was told it was docked and the picture size and short waits were chosen for a desktop PC
  - Now: on these devices the game is told it is in handheld mode and picks its own lighter handheld graphics settings, a window smaller than 1080p is no longer rendered at 1080p, controller prompts stay for the trackpad and touch screen, and (under Proton) short waits sleep instead of spinning. `pkp.log` shows which board rule matched in the `host: handheld` line
  - Still not: AYANEO, GPD and OneXPlayer, whose board names are not reliably known (OneXPlayer's also cover Intel models); set `[host] handheld = on` in `pkp.ini` for those. If the handheld graphics settings do not suit your device, set `[am] operation_mode = docked` (that alone), or `[host] handheld = off` (all of it)

### Steam Controller on Linux
- Settings > Controls now explains, on Linux only, how to get a Steam Controller or other pad working: add the game to Steam and keep Steam Input on for it, and what that means for the grips and trackpads

### Gameplay mods
- Settings > Gameplay and `pkp.log` now show which gameplay mods and experimental features are on, so you can see at a glance what a start will use
- Fixed the Pokédex tabs showing "Time" as their title after the Settings screen had been opened (#494)
- The Discord links in the launcher, the READMEs and the website now use the new invite, discord.recompia.com. Update only: no shader rebuild, no Set up again

### Online presence (opt-in)
- New **Share that I am online** switch, off by default, easy to find in three places: the **Online** chip on Home, the **Online** group at the top of Settings > Game, and a new **Online** tab in the game's own Settings screen (all three are the same switch, `[pkp] online_presence` in `pkp.ini`)
  - Before: the launcher sent nothing about you to any Recompia server
  - Now: while it is on and the game runs, the launcher tells the Recompia online server you are playing: an optional online name (empty: anonymous), your version, Windows or Linux, and how many Pokémon have a home in your save. Your save, profile nickname and user ID are never sent. Switching it off in the game takes you off the list within a few seconds. The first time you press Play, the launcher asks once whether to switch it on; the answer is no unless you pick yes. **Reset my online ID** (Settings > Game) starts a new ID. No shader rebuild; the game's Online tab comes with this build's **Set up again**
  - Still not: recompia.com has no page that shows who is online yet, and the online name can only be typed in the launcher

### The Testing tab
- Added a **Testing** tab to Settings, shown only on the Testing channel: send logs, read them, and switch the features still being tried on or off
  - **Export and report** makes one zip of `pkp.log`, `pkp.log.prev`, the crash files, `setup.log` and `pkp.ini`, opens its folder (`testing-logs` next to `pkp.exe`) and the bug report form. Drag the zip into the form: the launcher has no GitHub login, so nothing is uploaded for you
  - Names in paths, your account and server settings and IP addresses are removed before anything is written. Long logs keep their end, so the zip stays under GitHub's 25 MB limit
  - **View logs** shows the end of each file in the launcher, with a filter and a Copy button
  - **Experimental features** has **Mouse in the game's menus** and the **Taller view** on 16:10 screens, both off by default. The gameplay options (Camera aim, Running jump, Sort, Search, Quit Game) are off by default in this Testing build; switch them on here
- The touch screen **Menu button** is automatic: on when the machine has a touch screen, so a handheld keeps its way into the menu

### Link Play
- Updated the co-op module to pokopia-native 1.4.0. It still plays with pokopia-native 1.3.0 to 1.4.0 and with earlier builds of this game: the way games talk to each other did not change
- Added **automatic port forwarding** when you host over the internet without a relay
  - Before: your router had to forward port 47600 (TCP and UDP) by hand
  - Now: while you host, the game asks your router to forward it (UPnP, NAT-PMP or PCP) and gives it back when you stop. **Ask the router to open the port** on the Link Play page's Host view (`[net] port_forward`) turns it off
  - Still not: a router with UPnP switched off, or an internet provider that shares one address among many homes, can't be helped by this; `pkp.log` says which, in a `nat:` line. A relay you run on this PC still needs its own port forwarded. Quitting the game while hosting leaves the forwarded port until its one-hour lease runs out
- `pkp.log` no longer contains your whole room code, only its first group, so a log is safe to attach to a bug report. The launcher still shows the whole code

### Game
- NVIDIA graphics cards no longer risk a crash (device lost) in six of the game's compute shaders when a loop count is not the game's: the count is held to the size of the table the loop walks (on other cards, to 16384). From pokopia-native 1.4.0; `[nvn] loop_guard = off` turns it off
  - The lighting shader now has the guard in all four of its variants, not one. Two of the unguarded ones ran in a game that fell from 95 to 3 fps after 8 minutes on an RTX 3060 (#309)
- **Story co-op** (experimental, `[pkp] link_story_coop`): the host's own story no longer stops while friends are visiting. Before, any Link Play session turned the host's story talks into "Bring <host> here to say hi!" and paused its requests. From pokopia-native 1.4.0

### Launcher
- Added **Copy handheld report** to Settings > Testing (Testing channel only)
  - Before: a tester on an AMD handheld had to send the whole `pkp.log`, which holds names and paths, and say by hand what mattered
  - Now: one press copies a short block with only the useful lines of `pkp.log` (handheld detection, graphics card and driver, memory, the frame summaries and any graphics reset or crash), with user names, home folders, addresses and room codes removed, ready to paste into a bug report. Add your device, system, power mode and what you were doing
  - Still not: it does not send anything and does not replace **Export and report** when a full log is asked for
- Added **arrow-key** navigation to the launcher, for a handheld whose d-pad reaches it as keys
  - Before: Steam's default layout for a non-Steam shortcut sends the d-pad as arrow keys, and the launcher ignored them
  - Now: the arrow keys move between controls, Enter presses, Escape goes back (closes a question, or returns Home), the same as the pad
  - Text boxes, sliders and drop-downs keep the arrows they need
- New **Show on Discord while playing** in the launcher's Settings: shows "Playing Recompia" and how long you have played on your Discord profile while the game runs. Off by default; Discord must be open. Update only: no shader rebuild, no Set up again

### Gameplay
- Fixed **camera aim** staying off after you used the Joy-Con 2 mouse mode once (#367)
  - Before: turning mouse mode off left the game stuck half in it, so camera aim never came back until a restart
  - Now: pressing the mouse mode key again fully leaves mouse mode and camera aim works again right away
- Fixed the Joy-Con 2 mouse mode hitting the wrong block (#367)
  - Before: clicking broke or placed a block off to the side of the one under the pointer
  - Now: your character turns to the block under the pointer before swinging, so the click lands on it

### Controls
- Fixed the **Mouse Mode Key** row in **Settings > Mouse** (#367)
  - Before: pressing a key on the row did nothing, and after one change the row got stuck at the end of its bar
  - Now: press left or right on the row (or click it), the row says "Press the new key", then press any key, mouse button or wheel notch. Esc cancels. It works every time, and F1 can't be picked by mistake
- Added **Controller Button** and **Presses** rows to the game's own Settings > **Controller** tab: remap any controller button without the F1 page
  - Before: giving a rear grip, a trackpad click or another controller button a Switch button was only possible in Settings (F1) > Controller, or in `controls.ini`
  - Now: on Settings > Controller, press left or right on **Controller Button** to pick a button (for example Right grip R4), then on **Presses** pick what it presses on the Switch pad (Default, Nothing, A, B ...). Save as with any other row; Cancel keeps the old layout. A button the file maps to several buttons at once shows **Custom** and is left as it is
  - Still not: it shows one button at a time; the F1 page still lists them all
- Added two more bindable controller buttons: the **Steam button** and the **extra button** (the Steam Controller (2026)'s and the Steam Deck's quick-access "...", an Xbox Share, a PlayStation 5 microphone button) (Settings F1 > Controller > Pad buttons, or `guide` and `misc1` in `[padmap]` of `controls.ini`)
  - Before: they pressed nothing and could not be given a Switch button
  - Now: each has a row and can press any Switch button; with the Steam icons on, a prompt for a button you gave them shows an **S** or three dots. They press nothing until you pick one
  - Still not: with Steam running, Steam may keep these two buttons for itself (start the game outside Steam to use them); the Steam Controller (2026)'s trigger clicks are not here, because the controller library in this build does not report them
- Changed the **Steam Controller** button icons to fit the new Steam Controller (2026)
  - Before: the icons were the original 2015 controller's: its small buttons were the `<` and `>` arrows and it had two rear grips, so R4 and R5 (or L4 and L5) both showed "RG" (or "LG")
  - Now: View and Menu show the same symbols as on a Steam Deck, and each rear grip has its own icon: **L4**, **L5**, **R4** and **R5**
  - Still not: it needs the new `prompt_icons.pak` that comes with the build; the Steam button, the quick-access (...) button and the trigger clicks have no prompt icon yet


## 0.11.0 (build O: Ocelot)

**Update:** press **Update**, then **Set up again** once. It rebuilds the game code with a new, leaner translation, several minutes. Your shaders and saves are kept; no shader is translated again.

Ocelot brings Recompia's own **official relay server**, so friends anywhere in the world join with just a room code, and a big step up in **Steam Deck performance**. It also fixes the random "graphics device was lost" crash on Steam Deck and Linux, brings Joy-Con 2 mouse mode back and lets more Pokémon live in your town.

### Highlights
- **Official Recompia relay server:** internet Link Play now needs only a room code. The host shares the code, visitors pick Visit and type it, from anywhere in the world: no addresses, no port forwarding
- **Steam Deck performance, elevated:** about 52 fps where 0.10.3 gave about 44, and much shorter slow frames (the slowest 1 % now take about 26 ms, before about 38 ms). Measured on a Steam Deck OLED, in the world, vsync off
- Fixed the random **"The graphics device was lost"** crash on Steam Deck and other Linux PCs, and the full freeze on NVIDIA under Linux (#531, #533)

### Link Play
- The launcher's **Internet** choice now uses the official Recompia relay by default. Your own relay, this PC's relay and a direct address are still there under **Advanced**
- The docs have a step-by-step Link Play guide with the launcher's current names

### Performance
- The graphics work of a frame reaches the GPU in pieces while the rest is still being prepared, so the GPU starts several milliseconds earlier in each frame (Steam Deck and other Linux handhelds)
- Less processor time spent by the game's helper threads: fewer wake-ups, the driver's shader cache is written at most every 30 seconds, and a guard that could do nothing under Proton no longer runs there
- Shorter waits between the game's own threads on Linux handhelds: short sleeps end on time, and a wait ends as soon as what it waits for has happened
- Fewer redundant copies of graphics memory and less repeated work for each draw
- The game's own code is translated to leaner machine code: the game's registers and condition flags live in processor registers instead of memory wherever no other code can look at them. This is the change that needs **Set up again**
- On a Windows PC the frame rate is the same as in 0.10.3

### Frame pacing
- Fixed uneven frames with a frame limit: now and then one frame waited twice for the limiter and the next one not at all
- New, off by default: **vblank lock** for a frame limit of half the screen's refresh rate with vsync on, for example 45 fps on the Steam Deck OLED's 90 Hz screen. Every frame is then shown for the same number of refreshes instead of an occasional long frame followed by a short one. To try it: vsync on, frame limit 45, then `vblank_lock = 1` under `[latency]` in `pkp.ini`

### Display
- **Frame generation** (AMD FSR 3.1), added in 0.10.3, is removed for now: it has a bug that can't be fixed yet (#529)

### Gameplay mods
- New, experimental: a **Pokémon resident cap** up to 64. On by default at 31 residents (the game's own limit is 25); set it from 25 to 64 in the launcher's Settings > Gameplay, or with `pokemon_cap_limit` in `pkp.ini`. The game has 64 actor slots in all, so a high cap leaves less room for visitors and temporary spawns, and not every resident is sure to be out at once. Thanks to SpiderNic96 for the research and first implementation (#524)

### Controls
- **Joy-Con 2 mouse mode** is back: press the middle mouse button in play to point and click; placing and breaking land on the block under the pointer (left click is the Joy-Con's R, right click its ZR). Press it again to go back to camera aim. The **Mouse Mode Key** row is back in **Settings > Mouse**

### Launcher
- New **Show on Discord while playing** in Settings: shows "Playing Recompia" and how long you have played on your Discord profile while the game runs. Off by default; Discord must be open. Thanks to darkxex (#511)
- The launcher shows **Recompia** branding by default (#532)
- The Discord links in the launcher, the READMEs and the website use the new invite, discord.recompia.com
- When the shader translator closes without a word during Set up under Proton ("Translating shaders: 0 of 99012", #530), Set up now runs that batch again with lighter settings (workstation memory management, 2 jobs) and keeps them for the rest of the shaders. setup.log also shows the translator's last steps when it fails

### Game
- Fixed the random **"The graphics device was lost"** crash on Steam Deck and other Linux PCs (Proton), and the matching full freeze on NVIDIA under Linux (#531, #533)
  - Before: the game's copy of graphics memory could keep an old value next to a block the graphics card was still writing, and a compute shader then looped for seconds on it
  - Now: that memory is kept up to date, and the game's light-list and two other compute loops can no longer run past the table they walk. `[nvn] loop_guard = off` turns the second part off
- Fixed the Pokédex tabs showing "Time" as their title after the Settings screen had been opened (#494)

### For bug reports
- New lines in `pkp.log` every 10 seconds: `tcpu` (processor time by thread) and, with `present_stats = 1` under `[latency]`, `pacing: present timing` (how many refreshes each frame was shown for)
- If something breaks, these switches in `pkp.ini` bring back the earlier behaviour one at a time, which helps to find the cause: `gpu_split_ms = 0` and `pool_reg_skip_same = false` under `[nvn]`, `poll_wait = false`, `short_sleep = sleep` and `early_wake = off` under `[kernel]`, `boundary_count = render` under `[latency]`
- To compare against the old translation of the game code: set the environment variable `PKP_LOCALS=off` and press Set up again

## 0.10.4-alpha.1 (Deck Rehaul)

**Experimental build.** It is not offered as an update: download the zip from this release, or pick this version in the launcher on the Testing channel. It holds the performance work for the Steam Deck on top of 0.10.3. After installing it, press **Set up again** once: it rebuilds the game code with the new translation, several minutes. Your shaders and saves are kept; no shader is translated again. Please report anything that looks or plays differently from 0.10.3, with `pkp.log`

### Performance (measured on a Steam Deck OLED, in the world, vsync off)
- Higher and steadier frame rate: about 52 fps where the build before this work gave about 44, and the slowest frames are shorter (the slowest 1 % took about 26 ms, before about 38 ms)
- The graphics work of a frame now reaches the GPU in pieces while the rest is still being prepared, so the GPU starts several milliseconds earlier in each frame (Steam Deck and other Linux handhelds)
- Less processor time spent by the game's helper threads while you play: fewer wake-ups, the driver's shader cache is written at most every 30 seconds, and a guard that could do nothing under Proton no longer runs there
- Shorter waits between the game's own threads on Linux handhelds: short sleeps end on time, and a wait ends as soon as what it waits for has happened
- Fewer redundant copies of graphics memory and less repeated work for each draw
- The game's own code is translated to leaner machine code: the game's registers and condition flags now live in processor registers instead of memory wherever no other code can look at them. This is the change that needs **Set up again**

### Frame pacing
- Fixed uneven frames with a frame limit: now and then one frame waited twice for the limiter and the next one not at all
- New, off by default: **vblank lock** for a frame limit of half the screen's refresh rate with vsync on, for example 45 fps on the Steam Deck OLED's 90 Hz screen. Every frame is then shown for the same number of refreshes instead of an occasional long frame followed by a short one. To try it: set vsync on and the frame limit to 45, then add `vblank_lock = 1` under `[latency]` in `pkp.ini`
- On a Windows PC the frame rate is the same as in 0.10.3

### For testers
- The new translation of the game code cannot be switched off in `pkp.ini`. To compare, set the environment variable `PKP_LOCALS=off` and press Set up again: that builds the game code the way 0.10.3 does
- New lines in `pkp.log` every 10 seconds: `tcpu` (processor time by thread) and, with `present_stats = 1` under `[latency]`, `pacing: present timing` (how many refreshes each frame was shown for)
- If something breaks, these switches in `pkp.ini` bring back the earlier behaviour one at a time, which helps to find the cause: `gpu_split_ms = 0` and `pool_reg_skip_same = false` under `[nvn]`, `poll_wait = false`, `short_sleep = sleep` and `early_wake = off` under `[kernel]`, `boundary_count = render` under `[latency]`

## 0.10.3 (build N: Newt Hotfix a3)

**Update:** press **Update**, then **Set up again** once. It rebuilds the game code for the shoulder camera's new hook, a few minutes. Your shaders and saves are kept; no shader is translated again.

The Newt hotfix fixes a start-up crash on some AMD graphics, loading screens fast-forwarding, camera aim picking the wrong block and the launcher's Quality preset, and adds an over-the-shoulder camera, six button icon themes and a Gameplay tab in the game's own Settings.

### Highlights
- Fixed the game closing at start with 0xC0000409 on some AMD graphics (#487)
- Fixed loading screens and the boot screen fast-forwarding above 60 fps
- Added an **over-the-shoulder camera**

### Gameplay mods
- Every gameplay mod now has its switch in the launcher's **Settings > Gameplay**, built from the game's own list of mods, so a new mod shows up there by itself
  - **Search in the crafting screen** is now optional: off by default, switch it on in Settings > Gameplay. If you already set it yourself, your choice is kept
  - **Camera aim** has a new **Extra reach in blocks** setting under it (Off, 1 to 4). Extra reach is now off by default, so abilities and tools use the game's own range; pick a number to reach farther, as 0.10.x did with 2
  - **Sort each time the bag opens** stays under the Sort button, off by default
  - **Quit Game on the title screen** stays on, without a launcher switch (`title_quit` in pkp.ini still turns it off)
- Settings: a **Gameplay** tab in the game's own Settings screen, with the same switches as the launcher's Settings > Gameplay (and how much speed a running jump keeps); a change is saved to `pkp.ini` and applies at the next start

### Launcher
- Fixed **Quality preset** in the launcher leaving Shadow quality and Anti-aliasing as they were
  - Before: picking High, Ultra or Desktop PC in Settings > Graphics set only the texture settings (and the render resolution and frame limit for the device presets), so the game kept the old shadows and anti-aliasing
  - Now: the preset sets them too, as it does in the game: High gives High (2x) shadows and SMAA High, Ultra gives Very High (3x) and SMAA Ultra, Desktop PC gives Very High (3x) and SMAA High; the other presets give the game's own shadows and no anti-aliasing

### Game
- Added an **over-the-shoulder camera**: move the view to the side and up so your character no longer blocks it
  - Two distances in blocks in the game's Settings > **Gameplay** tab: **Shoulder camera: side** (right is positive, left negative, -2 to 2) and **Shoulder camera: height** (-1 to 2), saved as `camera_shoulder_side` / `camera_shoulder_height` in `pkp.ini`; both are 0 by default, which is the game's own camera. A change takes effect as soon as you leave the Settings screen, no restart. The view stops short of walls and eases back out, and camera aim still hits what the crosshair points at. Needs **Set up again** once (new game hook)
- Fixed camera aim's selection jumping between the block you aim at and the way you walk, with Rock Smash, Cut and the other swing tools, when the aimed block went in and out of reach while moving
  - Now: without a block in reach the selection stays in front of you in the camera's direction, at the game's own distance
- Camera aim reaches up and down as far as it reaches sideways: a block above or below you under the crosshair is selected within the same range (1 block plus the extra reach), also past the two rows up and one row down the game itself allows
- Fixed camera aim sometimes selecting a block next to the one under the crosshair, at some camera angles and heights
  - Before: for a block right next to you the game still picked the cell one straight step ahead, so a block diagonal to you (or at an angle that rounded the other way) wasn't the one selected
  - Now: the selection is the block the crosshair's ray hits, next to you or farther with extra reach
- Fixed the game closing at start ("pkp.exe exited with code -1073740791 (0xC0000409)") on some AMD graphics (#487)
  - Before: every compute shader asked the driver to run 32 wide, even one with a workgroup too big for that on AMD; Vulkan doesn't allow the request then, and AMD's driver could close the game while preparing it
  - Now: a compute shader whose workgroup doesn't fit at 32 wide runs at the driver's own width; the others still run 32 wide
- Fixed the boot screen and some loading screens fast-forwarding above 60 fps
  - Before: while the game loaded, the frame limit was dropped, so the loading animations ran as fast as the PC could draw (300 fps or more on a fast PC) and the game's 60 fps hold for loading and events was skipped
  - Now: loading screens keep the frame limit and the 60 fps hold, so they play at normal speed. A world load takes under a second longer. To bring the old behaviour back, turn on **Fast loading screens** in the launcher (Settings > Game > Experimental), or set `[display] unpaced_loading = 1` in `pkp.ini`
- Added six **Button Icons** themes: **Nintendo Switch**, **Nintendo Switch 2**, **Wii U**, **Steam Frame**, **Valve Index** and **Touch**, and the **Steam Deck** theme now uses real Steam Deck art
  - Before: the prompts could show the game's own icons, Xbox, PlayStation, Steam Controller, Steam Deck (the Steam Controller's art with Xbox View and Menu symbols) or keyboard and mouse
  - Now: pick any of them in the game's Settings (Button Icons), on the F1 Controller tab, in the launcher's Controls tab or with `[prompts] style` in `controls.ini` (`switch1`, `switch2`, `wiiu`, `steamframe`, `index`, `touch`). All are the white Kenney icons and follow your button remapping. Steam Deck shows the Deck's own L1/R1, L4/L5/R4/R5 back buttons and trackpads. Valve Index shows A, B, the triggers and the trackpad (its left stick stands for the d-pad); Touch shows gestures (tap, two-finger tap, swipes, pinch)
  - Also: the launcher's Controls tab (under Button icons) and the F1 Controller tab now show a preview of the chosen theme: each Switch button with the icon it gets. F1 follows your current remapping and key bindings; the launcher shows the default ones
  - Still not: Automatic never picks the new themes on its own (a Switch pad still shows the game's own icons); buttons a controller doesn't have keep the game's icon

## 0.10.2 (build N: Numbat Hotfix a2)

**Update:** press **Update**, then **Set up again** once. It rebuilds the game code for Clock Notation's new hook: a few minutes coming from 0.10.1, longer if you skipped 0.10.0's Set up. Your shaders and saves are kept; no shader is translated again.

The Numbat hotfix brings back the colourful logo, translates the launcher into nine languages, fixes an AMD start-up crash and BetterJoy input loss, and adds Time and Events tabs to the game's Settings.

### Launcher
- Set up now says why the shader translator stopped on Steam Deck and other Linux PCs (#366)
  - Before: under Proton the translator's error text never reached setup.log, so a failed "Translating shaders" step showed only an exit status and an empty "Last output"
  - Now: the translator writes its error to `shader_cache/stage/harness_error.txt` and Set up copies it into setup.log; if the translator never started at all, the log says Proton or Wine failed before it. Set up also checks that the translator's main files are present and not empty before it starts
- The colourful **Recompia** logo and the green **R** icon are back in the launcher, the README, the window and taskbar icon and the Steam shortcut (the plain placeholder logo and icons from 0.10.1 are gone)
- The launcher is now translated into Spanish, English, Japanese, French, Italian, German, Korean, Simplified Chinese and Traditional Chinese. It follows the system language by default (English when it cannot tell), and Settings > About > Language changes it
- Fixed Set up finishing at once while the game still refused to start with "pkp.exe and pkp_rt.dll come from different releases", after an update that replaced only some files
  - Before: the launcher compared pkp.exe only with the build tools in the tools folder. A new pkp_rt.dll next to old tools (files copied by hand, or a zip extracted without its tools folder) looked current, so Set up did nothing and the game refused the exe
  - Now: pkp.exe and pkp_rt.dll both carry the runtime API they were built for, and the launcher reads it before Play and Set up. An exe from another release than the dll is relinked by Set up; a dll from another release than the tools is named on the main page, with what to do (extract the whole release over the folder)

### Game
- Fixed the game closing right at start ("pkp.exe exited with code 4") on some PCs with an AMD graphics card
  - Before: when an older AMD driver was still installed next to the current one (common with a Ryzen processor's own graphics), the card was listed twice and the game could pick the old driver's copy, which crashed inside the driver on the first frames
  - Now: the copy with the older driver is skipped and the log says so; reinstalling the graphics driver removes the leftover
- Added a **Time** tab and an **Events** tab to the game's own Settings screen, for moving the game's clock without restarting (#385)
  - Before: the game's date and time came from the PC, and the launcher's event preset or `clock_offset` only applied at the next start
  - Now: **Time** has Time Travel (Off follows the real time) and Year, Month, Day, Hour and Minute rows; the clock changes when you leave the screen. **Events** lists the events of `pokopia_events.tsv` (one line each: start date, end date and name, separated by tabs); pressing one sets the clock to noon on its first day, marks it **Chosen** (one at a time) and fills the Time tab. Nothing is written to your save or to Settings
  - Still not: tab icons of their own (both tabs use the Game tab's), and a launcher button that fetches the event list
- Added **Clock Notation** to the Time tab: Game, 12 hour or 24 hour for the time of day the game shows
  - Before: the game's language chose the notation (English showed 12 hours with AM/PM whatever the PC's region), and nothing in the settings changed it
  - Now: pick 12 hour or 24 hour (`[tweaks] clock_format` in `controls.ini`: `auto`, `12h` or `24h`); Game keeps the language's own. A screen that is already open shows the new notation after you close it and open it again
  - Still not: a launcher row for it
- Button prompts: the face buttons are plain white, like the rest of the prompt icons
  - Before: Xbox, PlayStation, Steam Controller and Steam Deck prompts drew A/B/X/Y and cross/circle/square/triangle as coloured buttons
  - Now: they use Kenney's monotone white buttons, matching the bumpers, triggers, d-pad and keyboard icons. Switch prompts are unchanged
- Fixed the inventory **Sort** prompt showing **L** on some screens, where L also turns the page to the tab on the left
  - Before: where Y and R both had an action, the Sort prompt took L; pressing it sorted the panel and switched the tab at the same time
  - Now: Sort is Y in the bag and R in container screens; where both are taken the prompt is click only (left click or the menu pointer), and L only turns the page
- The F1 menu opens from a controller by clicking both sticks (L3 + R3) (#364)
  - Before: no controller shortcut was set, and the README's example (L + R) is the game's Dive chord, so holding R and L to dive opened the menu instead
  - Now: `[overlay] pad` defaults to `left_stick+right_stick`. A `controls.ini` from an earlier release keeps the value it already has (`none` = off); set the key to `left_stick+right_stick` there, or delete the line, to get the shortcut
- Fixed a controller dropping stick and button input during vacuuming, breaking and Rollout above 30 fps when it goes through BetterJoy (#374)
  - Before: the game sent the controller a new rumble value on almost every frame while it rumbled, about 100 times a second at 100 fps. BetterJoy passes each one to the controller over Bluetooth and stopped reading the stick and buttons while it caught up
  - Now: rumble goes to the controller at most 30 times a second at any frame rate, the rate that already worked at a 30 fps cap
- The "different releases" message names the runtime API of pkp.exe and of pkp_rt.dll and points to the launcher's main page, since Set up alone cannot always help

### Docs
- The README and the Linux readme are now also in Brazilian Portuguese and European Portuguese, and the README's F1 controller example uses L3 + R3 instead of the Dive chord (L + R)

## 0.10.1 (build N: Narwhal Hotfix a1)

**Update:** press **Update**. No **Set up again** is needed (your saves and shaders are kept).

This is a small hotfix, mainly a compliance update: the launcher's logo and icons are plain ones, the README pictures are placeholders, and the code no longer carries the earlier game's name. The launcher will get a new look soon.

### Launcher
- Changed the logo and icons to plain ones, and took the earlier game's name and a personal folder path out of comments and scripts
  - Before: the logo and icons were multi-colour bubble letters like the game's
  - Now: the logo is plain "Recompia" text and the icons are an R on a rounded square (Testing: with a T badge); the README pictures are plain placeholders
  - Still not: the launcher's **Koi's Camp** colours and the Steam shortcut pictures keep Koi's hand-drawn picture
- Added **Meadow**, a fourth launcher colour with an original scene made for the project (hills, trees, a pond and a path), next to Sunrise, Dusk and Koi's Camp
  - Before: Koi's Camp was the only daytime picture
  - Now: pick **Meadow** with the Colours button; the new picture is about 100 KB, and the Steam shortcut pictures are lighter 256-colour PNGs
  - Still not: a custom-drawn look from an artist, which comes with the visual refresh

## 0.10.0 (build N: Narwhal)

**Update:** press **Update**, then **Set up again** once (new game hooks for the crafting Search button; your saves and shaders are kept). This first Set up after the update may be a full one and take as long as usual; later runtime-only updates relink in seconds.

The Narwhal patch sharpens the picture and smooths the handhelds: anti-aliasing and sharper shadows, no more hitch once a second at a frame cap, a Search button for crafting, and DLC added in one click.

### Highlights
- Added a **Search** button to the crafting menu: type part of an item name and only those recipes are listed (#324)
- Added **Anti-aliasing** (SMAA) and **Shadow quality** (up to 8x sharper shadow maps), both picked by the Quality preset too
- Fixed the **hitch about once a second** with a frame cap on the Legion Go S and Steam Deck (#276)
- Fixed new Pokémon and areas staying **invisible until a restart** (#304)
- Added **Add DLC** to the launcher and a **Steam Deck** button icon style
- Fixed **Repair** and **Undo** in the launcher, which could not run

### Known issues
- The **Joy-Con 2 mouse mode** is switched off for now (it aimed at the wrong blocks on PC); camera aim is not affected
- Crafting Search does nothing during Link Play
- Shadow quality Ultra and Extreme need a lot of video memory

### Game
- Added a **Search** button to the crafting menu (#324)
  - Before: finding a recipe meant scrolling through the tabs
  - Now: press **Y** (keyboard **F**) or click **Search** in the button bar. The same keyboard as name entry opens (works with a controller: d-pad or stick to pick keys, A to type, + for OK), type part of an item name and the list shows only the recipes whose item name contains it, in your game language. Upper and lower case don't matter
  - The search stays on when you change tabs or use the game's Sort, and the button shows the word (for example "Search: log"). Cancel on the keyboard shows every recipe again; closing the crafting screen (or crafting something) clears the search
  - Want it gone? `[pkp] craft_search = 0` in `pkp.ini`
  - Still not: no search during Link Play, and only the item name is searched (not materials or descriptions)
- **Joy-Con 2 mouse mode** is switched off for now: it aimed at the wrong blocks on PC. The middle mouse button no longer turns it on and the **Mouse Mode Key** row is gone from **Settings > Mouse**. Camera aim is not affected

### Graphics
- Added **Anti-aliasing** (SMAA) for the 3D scene: the game's Settings > Performance > **Anti-Aliasing**, or Settings (F1) > Display > Upscaling > **Anti-aliasing**
  - Before: the game had no anti-aliasing here, so edges looked jagged
  - Now: pick **SMAA Low**, **Medium**, **High** or **Ultra** and press **Apply and save** (no restart). Only the 3D scene is filtered; the menus and HUD stay sharp. Default (none) is as before. Also `[upscale] aa = off | smaa_low | smaa_medium | smaa_high | smaa_ultra`
  - Still not: not hardware multisampling (MSAA); not applied together with a Scene Resolution below 100% and an upscaler
- Added **Shadow quality**: sharper shadows (Settings (F1) > Graphics > **Shadow quality**, right under Texture anisotropy, or the game's Settings > **Shadow Quality**)
  - Before: shadows were drawn from a 2048 x 2048 map, so their edges looked blocky up close
  - Now: pick **High (2x)**, **Very High (3x)**, **Ultra (4x)** or **Extreme (8x)** to make the shadow maps that many times larger per side. It applies at once, no restart. Default is as the game draws them. Also `[display] shadow_quality = default | high | very_high | ultra | extreme`
  - The game's soft shadow edge is made of 16 samples a texel of the original map apart; on a larger map the gaps between them showed as bands. With any option above Default each of those samples is now the average of 3 x 3 samples, which keeps the edge smooth. `[nvn] shadow_blur = false` turns that off
  - **Quality preset** now sets it with the anti-aliasing: High gives High (2x) and SMAA High, Ultra gives Very High (3x) and SMAA Ultra, Desktop PC gives Very High (3x) and SMAA High; the others give Default and no anti-aliasing. Changing either by hand makes the preset Custom
  - Still not: Ultra and Extreme use a lot of video memory (a 4x map is 128 MiB, an 8x one 512 MiB, and the game makes several); a size the GPU can't make leaves that map as the game made it
- Fixed the **Nearest** upscaler with **Scene Resolution** (#232)
  - Before: Nearest gave sharp pixels only with Resolution; with Scene Resolution the 3D scene was still stretched smooth and blurry
  - Now: with Scene Resolution below 100% the 3D scene is enlarged with hard pixels too, and the menus and HUD stay at full size
  - Still not: bloom and other soft glow effects stay smooth on purpose
- Fixed new Pokémon and areas staying **invisible until a restart** (#304)
  - Before: when the graphics driver briefly refused to map more game memory (`importing pool ... failed (-2)` in `pkp.log`), every model whose data lay in that part of memory stayed invisible for the rest of the session
  - Now: that memory is tried again a moment later (after half a second, then up to every few seconds), and imports nothing has used for a while are released first to make room, so the models appear once the driver has room again. `pkp.log` says when a retry worked

### Performance
- Fixed the long frame about once a second with a frame limit, on handhelds such as the Legion Go S and Steam Deck (#276)
  - Before: about once a second the game reads a tiny 30x16 picture of the scene back from the graphics card to work out the scene's average colour. Its main thread waited for that read (about 21 ms on a Legion Go S at 40 fps), so one capped frame in every forty missed its slot and took twice as long: a visible hitch while walking or turning the camera. The 0.8.0 change had only helped without a frame limit
  - Now: that read gets the previous second's picture at once and the new one is fetched in the background, so the main thread never waits. On the console the game reads that picture straight from memory without waiting for the graphics card either, so this is what it did there. Legion Go S at a 40 fps cap: frames over twice the median per 10 s 8.8 -> 0.2, 1% low 18-20 -> 21-34 fps. `[nvn] readback_defer = false` in `pkp.ini` brings the waiting read back
  - Still not: the scene's average colour, which tints the picture slowly, now follows a change of scene about a second later than before; the frame rate itself is as before

### Controls
- Added a **Steam Deck** button icon style, and Automatic now picks it on a Steam Deck
  - Before: on a Deck, Automatic showed the Steam Controller icons, so Select and Start were the small `<` and `>` arrows; the Deck's buttons are View and Menu
  - Now: **Steam Deck** (F1 > Controller, the launcher's Controls tab, or `[prompts] style = steamdeck` in `controls.ini`) shows the Steam icons with the Xbox-style View and Menu symbols. Automatic uses it for a Steam pad on a handheld; **Steam Controller** stays for a real Steam Controller
  - Still not: only View and Menu differ from the Steam Controller set; the grips and trackpads keep the Steam icons. It needs the new `prompt_icons.pak` that comes with the build

### Launcher
- Added **Add DLC** to the launcher and a **Steam Deck** button icon style
- Fixed **Repair** and **Undo** in the launcher, which could not run

### Known issues
- The **Joy-Con 2 mouse mode** is switched off for now (it aimed at the wrong blocks on PC); camera aim is not affected
- Crafting Search does nothing during Link Play
- Shadow quality Ultra and Extreme need a lot of video memory

### Game
- Added a **Search** button to the crafting menu (#324)
  - Before: finding a recipe meant scrolling through the tabs
  - Now: press **Y** (keyboard **F**) or click **Search** in the button bar. The same keyboard as name entry opens (works with a controller: d-pad or stick to pick keys, A to type, + for OK), type part of an item name and the list shows only the recipes whose item name contains it, in your game language. Upper and lower case don't matter
  - The search stays on when you change tabs or use the game's Sort, and the button shows the word (for example "Search: log"). Cancel on the keyboard shows every recipe again; closing the crafting screen (or crafting something) clears the search
  - Want it gone? `[pkp] craft_search = 0` in `pkp.ini`
  - Still not: no search during Link Play, and only the item name is searched (not materials or descriptions)
- **Joy-Con 2 mouse mode** is switched off for now: it aimed at the wrong blocks on PC. The middle mouse button no longer turns it on and the **Mouse Mode Key** row is gone from **Settings > Mouse**. Camera aim is not affected

### Graphics
- Added **Anti-aliasing** (SMAA) for the 3D scene: the game's Settings > Performance > **Anti-Aliasing**, or Settings (F1) > Display > Upscaling > **Anti-aliasing**
  - Before: the game had no anti-aliasing here, so edges looked jagged
  - Now: pick **SMAA Low**, **Medium**, **High** or **Ultra** and press **Apply and save** (no restart). Only the 3D scene is filtered; the menus and HUD stay sharp. Default (none) is as before. Also `[upscale] aa = off | smaa_low | smaa_medium | smaa_high | smaa_ultra`
  - Still not: not hardware multisampling (MSAA); not applied together with a Scene Resolution below 100% and an upscaler
- Added **Shadow quality**: sharper shadows (Settings (F1) > Graphics > **Shadow quality**, right under Texture anisotropy, or the game's Settings > **Shadow Quality**)
  - Before: shadows were drawn from a 2048 x 2048 map, so their edges looked blocky up close
  - Now: pick **High (2x)**, **Very High (3x)**, **Ultra (4x)** or **Extreme (8x)** to make the shadow maps that many times larger per side. It applies at once, no restart. Default is as the game draws them. Also `[display] shadow_quality = default | high | very_high | ultra | extreme`
  - The game's soft shadow edge is made of 16 samples a texel of the original map apart; on a larger map the gaps between them showed as bands. With any option above Default each of those samples is now the average of 3 x 3 samples, which keeps the edge smooth. `[nvn] shadow_blur = false` turns that off
  - **Quality preset** now sets it with the anti-aliasing: High gives High (2x) and SMAA High, Ultra gives Very High (3x) and SMAA Ultra, Desktop PC gives Very High (3x) and SMAA High; the others give Default and no anti-aliasing. Changing either by hand makes the preset Custom
  - Still not: Ultra and Extreme use a lot of video memory (a 4x map is 128 MiB, an 8x one 512 MiB, and the game makes several); a size the GPU can't make leaves that map as the game made it
- Fixed the **Nearest** upscaler with **Scene Resolution** (#232)
  - Before: Nearest gave sharp pixels only with Resolution; with Scene Resolution the 3D scene was still stretched smooth and blurry
  - Now: with Scene Resolution below 100% the 3D scene is enlarged with hard pixels too, and the menus and HUD stay at full size
  - Still not: bloom and other soft glow effects stay smooth on purpose
- Fixed new Pokémon and areas staying **invisible until a restart** (#304)
  - Before: when the graphics driver briefly refused to map more game memory (`importing pool ... failed (-2)` in `pkp.log`), every model whose data lay in that part of memory stayed invisible for the rest of the session
  - Now: that memory is tried again a moment later (after half a second, then up to every few seconds), and imports nothing has used for a while are released first to make room, so the models appear once the driver has room again. `pkp.log` says when a retry worked

### Performance
- Fixed the long frame about once a second with a frame limit, on handhelds such as the Legion Go S and Steam Deck (#276)
  - Before: about once a second the game reads a tiny 30x16 picture of the scene back from the graphics card to work out the scene's average colour. Its main thread waited for that read (about 21 ms on a Legion Go S at 40 fps), so one capped frame in every forty missed its slot and took twice as long: a visible hitch while walking or turning the camera. The 0.8.0 change had only helped without a frame limit
  - Now: that read gets the previous second's picture at once and the new one is fetched in the background, so the main thread never waits. On the console the game reads that picture straight from memory without waiting for the graphics card either, so this is what it did there. Legion Go S at a 40 fps cap: frames over twice the median per 10 s 8.8 -> 0.2, 1% low 18-20 -> 21-34 fps. `[nvn] readback_defer = false` in `pkp.ini` brings the waiting read back
  - Still not: the scene's average colour, which tints the picture slowly, now follows a change of scene about a second later than before; the frame rate itself is as before

### Controls
- Added a **Steam Deck** button icon style, and Automatic now picks it on a Steam Deck
  - Before: on a Deck, Automatic showed the Steam Controller icons, so Select and Start were the small `<` and `>` arrows; the Deck's buttons are View and Menu
  - Now: **Steam Deck** (F1 > Controller, the launcher's Controls tab, or `[prompts] style = steamdeck` in `controls.ini`) shows the Steam icons with the Xbox-style View and Menu symbols. Automatic uses it for a Steam pad on a handheld; **Steam Controller** stays for a real Steam Controller
  - Still not: only View and Menu differ from the Steam Controller set; the grips and trackpads keep the Steam icons. It needs the new `prompt_icons.pak` that comes with the build

### Launcher
- Added **Add DLC** (on the Setup page, or drop the file in the launcher): unpack the add-on of a DLC `.nsp` without building it by hand
  - Before: DLC had to be unpacked and put in `aoc/<add-on id>/romfs/` yourself, and the game saw none until you did
  - Now: press **Add DLC** and pick the `.nsp` (for the Bubbly Basin key, `Pokopia DLC [0400A3A01EE4D001].nsp`). The launcher unpacks its RomFS into the folder `[fs] aoc_dir` names, or into `aoc` next to the game and sets `aoc_dir` itself when `pkp.ini` names none. The game lists the add-on at its next start
  - Still not: the `.nsp` must already be decrypted, like the game files (an encrypted one is refused in words); no check of the IVFC hashes
- Settings > **Gameplay** now lists every gameplay option, each on by default
  - Before: only Camera aim and Running jump were there; the Sort button, crafting Search and Quit Game could only be switched off in `pkp.ini`
  - Now: one switch each for Running jump, Camera aim, Sort button (plus **Sort each time the bag opens**, off by default), crafting Search and Quit Game on the title screen. A gameplay option added later shows up here by itself
- The sidebar can now be hidden so the artwork is unobstructed: a **Hide sidebar** button next to the theme switcher collapses it, and a **Show sidebar** button in the corner brings it back. The choice is remembered per PC
- **Set up again** after an update that changes only the runtime now just relinks the game (seconds instead of translating and compiling it again); it keeps the compiled game libraries (a few hundred MB) in `build/lib`
- Backups, restore and downgrade now find the save folder the same way as Import and the game: when `pkp.ini` lists `[fs] save_dir` (or `[account] uid`) twice, the last one counts
- Fixed **Repair** and **Undo** on the Setup page: they could not start
- Fixed the backup made before a **downgrade** copying the wrong save folder
- Fixed the launcher crashing during the save check when a save's info file held non-English text

### Under the hood
- The code was reorganised from top to bottom so new features have a clean place to go. You should notice nothing, except:
  - Settings (F1) and `controls.ini` are shorter: old settings that did nothing are gone. Your old `controls.ini` still loads; the removed lines are dropped the next time it is saved, and the old `confirm` / `back` bindings become `btn_a` / `btn_b`
  - The release package no longer carries developer test tools

## 0.9.0 (build M: Monkey)

**Update:** press **Update**, then **Set up again** once (new game hooks for Quit Game, Sort, the mouse mode key and the longer camera aim reach; your saves and shaders are kept).

The Monkey patch is all about quality of life: tidy bags in one press, aim where you look from farther away, finally use the Joy-Con 2 mouse mode, and play from a handheld without ever touching a keyboard.

### Highlights
- Added a **Sort** button to your bag and every storage box: one press stacks everything up and puts it in order (#323)
- Fixed the **Joy-Con 2 mouse mode**: middle mouse now really turns on the game's own pointer, so you can point and click to break and place blocks, and you can bind it to any key (#367)
- Changed **camera aim** and **running jump momentum** to on by default, and camera aim now reaches 2 blocks farther
- Added full **controller and touch** support for handhelds: drive the launcher with a pad, tap a Menu button for the in-game settings, and map the Steam Controller's back grips and trackpads

### Known issues
- While the Joy-Con 2 mouse mode is on, the game holds you on Rock Smash and you can't swap tools: that's how the game's own mouse mode works. Press the mouse mode key again to swap
- Sort does nothing during Link Play, to keep both players' bags in step
- The Close the game? box on the title is the PC port's own, so it doesn't look like the game's menus yet

### Game
- Added a **Sort** button to the bag and to storage boxes (#323)
  - Before: tidying meant moving items around one by one
  - Now: press **Y** (keyboard **F**) or click **Sort** in the button bar. Half-full stacks of the same item merge (up to 99), then everything lines up by type with the empty slots at the end. Nothing is ever lost
  - In storage boxes **Y** is the game's own Add Stack / Remove All, so Sort is on **R** there (keyboard **Space**)
  - Sort tidies the panel your cursor is on. Holding **X** still rearranges like before
  - Want it gone? `[pkp] inventory_sort = 0` in `pkp.ini`. Want the bag sorted every time you open it? `[pkp] inventory_autosort = 1`
- Added **Quit Game** to the title screen, next to Settings (#319)
  - Press **(+)** (keyboard **Esc**) or click it: Quit closes the game, Cancel takes you back
  - Shown in your game language. `[pkp] title_quit = 0` in `pkp.ini` hides it

### Gameplay
- Changed **camera aim** and **running jump momentum** to on by default (#368)
  - Camera aim: abilities and tools hit where your camera looks, not just where your character faces
  - Running jump momentum: a running jump carries your running speed instead of slowing to the game's fixed air speed. Jump height is the same
  - Don't like them? Turn them off in **Settings > Game > Gameplay**, or `[pkp] camera_aim = 0` / `jump_momentum = 0`. If you had switched one off already, it stays off
- Added 2 blocks of reach to camera aim (#370)
  - Before: camera aim only reached the block right next to you
  - Now: look at a block up to 3 away and Water Gun, Leafage, Rototiller, Rock Smash, Cut, their upgrades and block placing all land there
  - The A button for talking and picking things up keeps the game's normal range. `[pkp] camera_aim_reach` sets the extra blocks (0 to 4, default 2)
- Removed the "always in Joy-Con mouse mode" behaviour that came with camera aim in 0.8.2
  - Camera aim now aims like in 0.8.1 and your tool is never switched for you
- Fixed the **Joy-Con 2 mouse mode** key doing nothing (#367)
  - Before: middle mouse freed your cursor, but the game never switched to its pointer mode, whatever key or trigger you picked
  - Now: middle mouse turns on the game's own pointer, just like laying a Joy-Con 2 on its side on a Switch 2. Point, left click to break and place, press middle mouse again to go back to the camera. The pointer lines up at any resolution, and camera aim steps aside while it's on
  - Rebind it to any key, mouse button or wheel notch in the game's **Settings > Mouse > Mouse Mode Key** (move the row, then press the key; Esc cancels), or on the F1 **Controls** page

### Graphics
- Removed the **view distance** setting: the world is always drawn as far as on the console (#249)
  - Before: anything above 1x made shadows follow the camera, put seams in block textures and left pictures in frames blank until you walked up
  - Now: always 1x. The slider is gone from every menu and preset, and an old `view_distance` line in your ini is ignored

### Controls
- Added support for the new **Steam Controller** (and other Valve pads): it's recognised as its own controller
- Added **Steam Controller button icons** (art by Kenney, CC0)
  - Pick them under Button Icons (launcher Controls tab, or F1 > Controller), or `[prompts] style = steam` in `controls.ini`
  - Automatic picks them when the game sees the Steam Controller directly. With Steam Input on, Steam shows the game an Xbox pad, so you get Xbox icons
- Added the Steam Controller's **back grips and trackpad clicks** to button mapping
  - Six new rows under Controls > Button layout (launcher and F1): R4, L4, R5, L5, the left trackpad click (also the PlayStation touchpad click) and the right trackpad click
  - They do nothing until you give them a button, so nobody's setup changes
- Added **touch screen** support (Steam Deck, ROG Ally, Legion Go, touch laptops)
  - A **Menu** button in the bottom left corner opens the F1 settings, so a handheld never needs a keyboard. The menu gets bigger rows, buttons and scroll bars
  - Taps still work the game's menus, but a tap during play no longer swings your tool, and touching the screen no longer swaps the button icons to keyboard keys
  - On by default when a touch screen is found. `[overlay] touch_button = off` turns it off

### Launcher
- Added **controller navigation** to the launcher (great for Steam Deck, ROG Ally and other handhelds)
  - D-pad or left stick moves, **A** presses, **B** goes back, **LB / RB** switch pages, the right stick scrolls
  - Sliders change with left and right, drop-downs with up and down
  - A thick focus ring shows while you use a pad and hides when you touch or click
- Changed the launcher on touch screens: no more double-tap zoom or tap delay

### For developers
- `runtime/games/pkp/pkp_play.cpp`: mid hooks 0x71002fa6f8 / 0x71002fa8a0 / 0x71002fa8ec let the game's mouse-mode check (0x71002fa6ac: two Joy-Cons + the save's mouse option) through for the local player while the Mouse mode key's request is on; `jc2_mouse_set_pointer_space` gives Nmouse x, y in the game's screen size; `GameHost` gains the rebind capture (`begin_capture` / `capture_result`)
- nvn: an opt-in, off-by-default experiment for the recurring small-texture readback hitch (#276): completion-polled readback with private buffers. Not a fix yet
- Release workflow: the third-party base payload now comes from our own v0.8.2 release (the upstream v0.1.1 repository is gone); the AppImage bundling retries up to 3 times

## 0.8.2 (build L: Lemur)

**Update:** press **Update** in the launcher, then **Set up again** once when it asks (camera aim adds hooks, so the game is rebuilt; your saves and shaders are kept).

### Highlights
- Added **Add to Steam** (Settings > Game > Steam): put Recompia into your Steam library, with its artwork, so it shows in Game Mode and Big Picture on a Steam Deck or any PC
- Added **Koi's Camp**, a new look for the launcher: a bright hand-drawn picture behind it, by Koi (press **Colours** in the sidebar to try it). Koi drew it while the bug fixes and the new features kept Jerban busy, so thank you, Koi, and sorry for the wait
- Camera aim now runs the game in its Joy-Con mouse mode during normal play (camera aim is still **off** by default, see Gameplay). Everything else in the game is as in 0.8.1. Sunrise is still the launcher's default look

### Gameplay
- Camera aim now runs the game in its Joy-Con mouse mode during normal play
  - Before: camera aim turned the aim to where the camera looks, and the game stayed in its normal (facing) mode
  - Now: with camera aim on, the game is in its Joy-Con mouse mode during normal play with any controller or the mouse, whatever its own mouse option says. The pointer stays at the centre of the screen and the camera still turns with the mouse and the right stick. Camera aim's targeting applies in both modes, your tool is not switched to Rock Smash, and the game's own pointer is not shown
  - Still not: camera aim stays off by default (**Settings > Game > Gameplay** or `[pkp] camera_aim = 1`); the Mouse mode key (free pointer) leaves mouse mode while it is on; only the first player is covered

### Launcher
- Added **Settings > Game > Steam**: put Recompia into your Steam library with its artwork, so it shows in Game Mode and Big Picture
  - Before: you had to add a Non-Steam Game by hand in Desktop Mode, set the launch option `--play` yourself, and find or make the pictures
  - Now: two buttons. **Game in Steam** adds a shortcut named Recompia that starts the game at once; **Launcher in Steam** adds one named Recompia Launcher that opens this window first. Both come with the artwork, drawn by Koi (the Testing build uses the Testing icon and says Testing in the name). Close Steam first and start it again afterwards. Remove takes the shortcut and its pictures out again; your other shortcuts are not touched, and a copy of Steam's shortcut list as you had it is kept the first time (`shortcuts.vdf.recompia-backup`, never overwritten).
- Added an optional **Koi's Camp** look: a bright daytime picture behind the launcher (artwork by Koi, hand drawn)
  - Before: the colours button switched between Sunrise and Dusk, both with the same island picture
  - Now: the button (**Colours** in the sidebar) goes Sunrise, Dusk, Koi's Camp. Sunrise stays the default, and nothing changes unless you press it
  - Still not: Koi's Camp has no setting of its own yet; it is picked with the colours button, and the launcher remembers it

## 0.8.1 (build L: Lynx)

**Update:** press **Update**, then **Set up again** once when it asks (this release adds hooks for the camera aim option and for the new in-game Options, so the game is rebuilt; your saves and shaders are kept). From 0.7.9 or older the same single Set up again covers everything.

### Highlights
- The PC settings are now in the game's own Settings screen: nine tabs, switched **on** by default (see Game)
- Added **Repair and reset** and **Choose a version** to Settings > About, and optional save backups with restore
- Added a camera aim option and a switch for running jumps that keep your speed; both are **off** by default, turn them on in **Settings > Game > Gameplay**
- Fixed doubled dialogue text above 1920x1080 (#267) and the story getting stuck above 60 fps (#297)

### Gameplay
- Added a camera aim option: abilities and tools aim where the camera looks instead of where your character faces, and your character turns to the target when an ability starts. It is **off by default**: turn it on in **Settings > Game > Gameplay** or with `[pkp] camera_aim = 1` in `pkp.ini`. Needs **Set up again** once (new hooks)
- Running jumps keeping your running speed (added in 0.8.0 as an ini option) is now a switch in **Settings > Game > Gameplay** too. It stays **off by default**; `[pkp] jump_momentum = 1` also turns it on

### Performance
- Added an option to read a small texture back without waiting, for the long frame about once a second with a frame limit on handhelds such as the Legion Go S and Steam Deck (#276). It is **off by default** and #276 is not fixed
  - Before: about once a second the game reads a tiny 30x16 texture back from the graphics card. Even after 0.8.0, its main thread waited for that read (about 21 ms on a Legion Go S, 13 ms of it waiting for the graphics card), so a capped frame missed its slot and took about 50 ms
  - Now (only with `[nvn] readback_defer = true` in `pkp.ini`): a small texture read again and again gets the data of the previous read (about a second old) and the new read finishes in the background, so the main thread does not wait
  - Still not: with the option on, the game froze for good 15 to 30 seconds after the start in our tests (3 of 3 runs), so it stays off until that is fixed. Please do not turn it on for normal play

### Fixes
- Fixed dialogue text showing up doubled and garbled when the picture is rendered above 1920x1080 (#267)
  - Before: with a render size such as 2880x1620 or 3392x1908 (for example a 2560x1440 window at 133%), the text of a dialogue box was drawn on top of the previous line, so words repeated and letters ran together. It showed most clearly behind the name prompt, where the game is paused
  - Now: the dialogue text is drawn once and clean at every 16:9 render size. The game keeps the size of its dialogue text buffer in a fixed 1400x1024; it is now made as large as the picture needs (1.5x at 2880x1620)
  - Tested at 2528x1422, 2880x1620, 3840x2160 and 3392x1908 (and unchanged at 2560x1080 ultrawide). Setting `[pkp] target_fix = off` keeps the old size; `[pkp] render_override = false` also avoids it
- Fixed the story getting stuck above 60 fps in DJ Rotom's mood 100 scene in Rocky Ridges: the dialogue disappeared after "YOOOOOO!" and the game never continued (#297)
  - Before: scripted events could wait forever when the game ran faster than 60 fps. Setting the frame limit to 60 got past it
  - Now: events, dialogue, transitions and menus always run at 60 fps as on the console, and free play keeps your frame limit. `[pkp] event_hold = false` turns this off
  - Still not: the exact timer that breaks above 60 fps is not known, so this holds the whole event at 60 fps instead of fixing that timer

### Game
- Added the PC settings to the game's own Settings screen (on by default; `[pkp] options_menu = 0` in `pkp.ini` brings back the original three tabs)
  - Before: the display, performance, graphics, picture, mouse, controller and sound settings were only on the F1 page and in the launcher
  - Now: Settings has nine tabs (Game, Camera and Sound, plus Display, Performance, Graphics, Picture, Mouse and Controller). L and R move through them, long lists and decimals are sliders, and one Save covers every tab. The values are the same ones F1 changes (`controls.ini`)
  - Still not: the FPS hotkey, quit key, keyboard bindings and pad remap stay on F1, and paths, setup and profiles stay in the launcher. English only; the six new tabs use the Game tab's icon for now
- Added lines to `pkp.log` for a long wait on the graphics card
  - Before: when the game suddenly dropped to a few frames a second, the log showed that it was waiting but not for what
  - Now: a `slow sync wait` line (at most 20 a session) is written when the game waits 100 ms or more, with the wait number, how long the game was willing to wait and where the graphics queue stood
- Added lines to `pkp.log` that say why the mouse pointer in menus is up or not
  - Before: when the mouse did nothing in a menu, the log could not say whether the window had lost focus, an overlay was open, the game was not in a menu, or the mouse setting was off
  - Now: `play: menu pointer gate` lines (at most 24 a session) show the state each time it changes, including whether `[mouse] menu_cursor` is on
- Fixed three small mistakes in the upscaler code that is switched off in this release (depth direction, number of jitter steps, when the picture history is reset). Nothing changes while the upscalers stay off
- Fixed `pkp.log` not saying which version you run
  - Before: the first lines of `pkp.log` read `pkp $env:VERSION` in every release, so a log could not be matched to a build
  - Now: the line shows the real version of the runtime (for example `0.8.1-beta.6`)
- Fixed a wrong line in `pkp.log` about the menu mouse
  - Before: it said the mouse pointer moves the game's cursor in menus. It does not: the wheel and the keys move the selection, and a left click confirms the selected entry
  - Now: the line says what the mouse does

### Launcher
- Changed the icon of the launcher program and its AppImage to an R on a green leaf shape
  - Before: the program icon was a plain game controller on a dark square
  - Now: the program icon (the file, the taskbar and the desktop) is the new R. Nothing inside the launcher window changes. Testing builds use their own icon (a hammer on the R)
- Added save backups: **Settings > Saves > Backups** copies your save every 5, 10 or 15 minutes while you play, so you can go back if a bug stops your progress
  - Before: a save that a bug had broken could only be fixed by restoring a copy you had made yourself
  - Now: pick a time (it is off until you do), and the launcher keeps the newest copies (10 by default) in `save-backups`, with one more copy when the game closes. **Back up now** makes a copy on request, **Restore** puts a chosen copy back with the game closed and copies your current save first so the restore can be undone. A copy is only made when no save is being written, and updates never touch `save-backups`

- Added **Settings > About > Choose a version**, and a list of Testing builds when you join the Testing channel
  - Before: Update only ever installed the newest build, and there was no way to read about a build before installing it or to go back
  - Now: the list shows the builds of your channel (Testing builds and the normal releases on Testing, only releases on Stable), newest first, each with its notes. A newer one installs like an update. Going back to an older one asks first; a long way back (an older release line) shows a stronger warning with a box to tick before it can go on. The launcher first copies your save folder, `pkp.ini`, `controls.ini`, `profiles.json` and `launcher.json` to `rollback-backups` next to the game (the last three are kept), and stops without changing anything if that copy cannot be made. Joining Testing opens the list of Testing builds right away
- Added a question when you switch the launcher to the Testing channel, and help for testers
  - Before: switching to Testing just switched, and nothing said what to do when something went wrong
  - Now: the launcher asks you to confirm, and says Testing builds can have bugs and that testers are expected to share what they find and their pkp.log. While on Testing, Settings > About has **Report a problem** and **Show pkp.log** buttons. Each Testing build's notes now list what to test and how to report, and the bug form asks which channel you were on
- Fixed a start from a shortcut (Steam, Sunshine) ignoring a profile problem
  - Before: if the launcher could not write the chosen profile into pkp.ini, a start with `--play` carried on anyway and the game could open another player's save
  - Now: that start stops with a message, like Play in the launcher window
- Fixed a damaged profiles.json being replaced without a trace
  - Before: a cut-off or unreadable profiles.json turned into one default profile, and adding a profile then overwrote the file, losing which save belonged to which profile
  - Now: the damaged file is first copied to `profiles.json.damaged-<date and time>` next to it, and if that copy cannot be made nothing is changed. Your save folders were never touched
- Added **Settings > About > Repair and reset** for when a setting or a leftover file causes trouble
  - Before: the only fix was to edit pkp.ini by hand or delete files
  - Now: choose what to fix (game settings, controls, launcher settings, caches, old copies of the runtime and launcher). You see the list of files before anything changes, and a copy of the changed settings files is kept, so **Undo last repair** puts them back. Your saves, profiles, shaders and game files are never touched
- Added a choice of what **Reset game settings** resets, in **Settings > About > Repair and reset**
  - Before: ticking Reset game settings put every game setting in `pkp.ini` back to the default, with no way to keep part of it
  - Now: ticking it opens a list with one tick per group: Graphics and performance, Network and Link Play, Gameplay options, Game clock and events, Save backup schedule, and Everything else. All are ticked, so doing nothing resets everything as before. Untick a group to keep it. The list of files you review shows which groups reset, and **Undo last repair** puts the whole file back. Your game folders, profile and Link Play identity are kept either way
- Added a Testing changelog to **What's New** for players on the Testing channel
  - Before: Testing builds were listed among the releases, each with the whole pending change list
  - Now: a **Testing** group with **All Testing changes** (every Testing build since the last stable release, newest first), what changed in each build, and its regressions and known problems as a highlighted block at the top. Stable players see no change
- Fixed the launcher feeling slow on Linux with an NVIDIA card: changing pages and scrolling lagged
  - Before: the Sunrise colours were a filter on the full-window background, which the launcher redrew on every page change. The picture itself was also much larger than the window needs
  - Now: the colours are part of the picture, which is smaller (about 180 KB for both themes instead of 430 KB for one), and a page change animates for 160 ms instead of 280 ms. It looks the same. On one test machine (RTX 4070 Ti, Wayland) the slowest frames of a page change (median of 6 runs each) went from 84 ms to 35 ms

## 0.8.0 (build L: Lykoi)

**Update:** press **Update**, then **Set up again** once when it asks (this release adds two hooks for the new jump option, so the game is rebuilt; your saves and shaders are kept). The new launcher and runtime come with it.

### Highlights
- Our first native gameplay change: running jumps can keep your running speed instead of flying at the old fixed speed, jump height unchanged (opt-in, see Gameplay)
- Added a **Community** row to Settings > About with a button that opens the Recompia Discord invite
- Added profiles: several people can play on one install, each with their own save (#282)
- Fixed a long frame about once a second on some handhelds and PCs (#276)
- Added a Testing update channel for players who want to try new builds early (opt in under Settings > About)
- Added a Recompia Discord server for help, testing and chat: **Settings > About > Community > Join Discord**

### Known issues
- The timed event toggles are experimental: in-game event activation has not been confirmed. Use them in single player only for now

### Gameplay
- Added a native gameplay change, the first one this project makes to the game itself rather than through a mod: running jumps keep your current speed instead of flying at the game's old fixed jump speed
  - Jump height is unchanged. Standing and walking jumps behave as before, and so do other players and Link Play visitors
  - It is off by default. Turn it on with `[pkp] jump_momentum = 1` in `pkp.ini`; `jump_momentum_strength` (0.0 to 1.0) makes the boost smaller. The boost is never more than double the old speed
  - Before: a running jump flew exactly as far as a walking one
  - Needs **Set up again** once because the game is rebuilt with two new hooks

### Performance
- Fixed a long frame about once a second on some handhelds and PCs: walking and turning juddered even when the frame rate looked fine (#276)
  - Before: the game reads a few tiny textures back from the graphics card about once a second. The read first waited a whole frame (about 17 ms at 60 fps) for the render thread to take it, which it almost never did, then forced the work through anyway. That held up the game's main thread
  - Now: the read starts the work at once. On a PC at 60 fps, long frames per 10 s: 4.2 ⇒ 0.0 (3 runs each), the slowest texture read: 79 ms ⇒ 6 ms
  - Setting: `[nvn] readback_grace_ms` is now 0 by default (`-2` brings back the old wait of one frame). `[nvn] readback_log = true` logs how long each read takes, for bug reports
  - Still not: confirmed on a Steam Deck or a Legion Go S only after this release. If you still see the hitch, please send a `pkp.log` with `readback_log = true`
- Changed the keyboard-icon check for the title screen so it no longer walks every texture once a second for the whole session (#280)
  - Before: with a keyboard or controller prompt style other than Switch, the game thread checked every loaded texture for the title art once a second, holding the texture list while it did
  - Now: the check slows down to every 10 s while nothing changes and speeds up again when textures load, so the title screen still gets its icon within a second
  - Still not: no hitch was tied to this check. `pkp.log` notes it when a check takes more than 2 ms

### Launcher
- Fixed picking a `.nup` made from a dump that holds only the `updates` folder: the launcher refused it, and the unpack progress could crash (#283, thanks julhin123)
- Added a **Community** row to Settings > About and a Discord chip on the Home page
- Added an update channel: **Settings > About > Update channel** switches between **Stable** (the default, what you get today) and **Testing** (newer builds, published as prereleases, for players who want to try changes early and report problems)
  - Before: every player got only the stable releases
  - Now: pick Testing to be offered test builds such as `0.8.1-beta.1`; **What's New** marks them as Testing. Your saves, profiles and settings are kept either way
  - Going back: the launcher only moves forward, so Stable offers an update again once it has a newer version than the one you have. See `docs/testing-builds.md`
  - Stable players see no change
- Added profiles: several people can play on one install, each with their own save (#282)
  - Before: a second save meant copying the whole game folder
  - Now: **Settings > Saves > Profiles** adds, renames and removes profiles, and **Home** gets a **Profile** picker once there are two. The game must be closed to switch
  - Your existing save is the first profile and stays as it is. A new profile starts with an empty save and its own Link Play player id
  - Profiles share the game files and the shaders, so a second profile costs almost no disk space
  - Removing a profile keeps its save in the save folder, nothing is deleted
  - Still not: screenshots, settings and controls are shared by all profiles
- Added one **Timed event** toggle each for the Copycat Challenge, More Spores for Hoppip, Bulbasaur's Jump Rope Contest and Sableye's Gem Hunt. Enabling one switches the others off. On every launch, Play applies that event's local 2026 start date at 09:00 to both game clocks without overwriting the ordinary Game clock, which is used again when all toggles are off. Use this in single player only for now. In-game event activation has not been tested.

## 0.7.9 (build K: Kiwi)

**Update:** press **Update**, then **Finish update** once when it asks (a shader translation fix: only the 32,630 changed shaders of about 99,000 are translated again, not the whole cache).

### Highlights
- Fixed the dashed outline missing around the tiles a move like Rock Smash or Water Gun will hit (#242)
- Fixed every Link Play visit failing with error 9005: saves made with this project all shared one player id
- Fixed a shader translation mistake in about a third of the game's shaders
- Fixed walking and camera turns juddering at a steady 60 fps (#275)

### Known issues
- Linux still runs the game and the shader setup through Proton
- Mods that replace files containing shaders are not supported; text and texture mods are the intended use
- Use the game clock in single player only for now

### Graphics
- Fixed walking and camera turns juddering about once a second at 60 fps or less, and constantly with the frame limit off and vsync on (#275)
  - Cause: the fix that keeps the game from running fast above 60 fps also shortened the step of any frame that ran short at 60 fps, such as the frame right after a late one. That frame still stays on screen for a full refresh, so movement lurched
  - Now: shorter steps apply only while the game runs above 60 fps (average over about 16 frames); at 60 fps or less the game's own rule is kept
- Windows on a small integrated GPU (including the ROG Ally): graphics-pool memory now uses the bounded cache already used under Wine, instead of keeping every imported chunk until Vulkan runs out of device memory and the picture freezes (#234)
- Fixed the dashed white outline missing around move target tiles and the block placement indicator; only the faint tint showed (#242, #120)
- Fixed one common shader instruction being translated wrongly in 32,630 of about 99,000 shaders
  - Other small details with the same cause (masks, patterns, dithering, effect switches) may look right now too
  - Something looks worse after this update? Open an issue with a screenshot and `pkp.log`
- Until you run **Finish update**, the game keeps the old translations and the outline stays missing
- Fixed shaders translated again by **Finish update** still loading their old translation from the boot preload pack (`shader_cache/spirv.preload.pack`), so about 6,000 of the most used shaders, the outline's among them, kept the old look
- Windows on Intel graphics: fixed the game crashing about 3 s after start (access violation in `igvk64.dll`, Intel UHD Graphics; #270)
  - Before: the game read its pipeline cache back from Intel's driver to save it, and the driver crashed while copying it out
  - Now: on Intel's Windows driver the pipeline cache is loaded but not saved (`[nvn] pipeline_cache_save`, `auto`); the driver's own disk cache still keeps compiled pipelines between starts
  - Still not: on Intel, Prepare shaders keeps nothing in `pipeline_cache.bin`, so pipelines the driver's cache doesn't have are built again at their first use

### Link Play
- Fixed every visit failing with "A communication error occurred" (error 9005), even with the room found and the same mods on both sides
  - Cause: every save made with this project got the same player id (UUID `b9ae85ed-...`), and the host turns away a visitor with its own id. The runtime's random seed (GetInfo RandomEntropy) was one fixed value on every PC, so the id the game rolls at new game came out the same for everyone
  - Now: the seed comes from the OS random source, so new saves get their own id
  - Saves you already have are repaired at the next start, before the game reads them: the shared id is replaced with a new random one in all its places. Progress is kept; a copy of the save from before goes to `uuid-backup\` in your save folder
  - `[pkp] uuid_repair = 0` turns the repair off
- Still cross-plays with pokopia-native 1.3.3: its co-op wire format, discovery, room codes and relay are unchanged since 1.3.0
- Joining a VPN address (Radmin VPN 26.x, Hamachi 25.x, Tailscale / carrier NAT 100.64.x) on LAN now names the VPN in the refusal and says to turn on internet play for every player
- Co-op module updated to pokopia-native 1.3.3 (`035bd9c`); its Interact everywhere and Story co-op sites for game 1.0.5 come along but do nothing here (this project needs 2.0.1). Based on pokopia-native (MIT) `windows/src/coop` @035bd9c

### Launcher
- Fixed Set up looking frozen on "Checking the setup tools..." or "Scanning game files: 29 of 29" for players whose shader cache was made before 0.7.0 (#269)
  - Before: Set up read every translated shader (about 99,000 files) one at a time to find the ones to translate again, with nothing on screen or in `setup.log`; on a slow disk that never finished in time
  - Now: the check reads on up to 8 threads, shows "Checking the shaders an earlier version translated: N of M" and writes its progress to `setup.log`; Pause stops it
- Fixed pages not scrolling in a small window with animations turned off (Windows: Animation effects off), so rows below **Mods** in **Settings > Game** were out of reach (#260)
- Added warm colour themes: the launcher opens in **Live sky**, which tints the island art by the time of day and the season; the **Colours** button at the bottom of the sidebar switches between Live sky, **Sunrise** (amber to red) and the old violet **Dusk**, kept per PC
  - Status colours: OK is blue instead of green, so OK, warning and error stay apart with red-green colour blindness

### For developers
- `src/kernel/svc_info.cpp`: GetInfo RandomEntropy is drawn once per process from the OS random source (`tt::rng_fill`, so the test tools' deterministic mode stays seeded); `[process] entropy_seed` still fixes it. It seeds nn::os's generator and with it nn::util::GenerateUuid (main 0x5af08c, the new-game reset 0x5aef60)
- `games/pkp/pkp_save_uuid.cpp`: at play install (before the game modules load) a save whose CMNDAT/SYDAT holds `b9ae85ed-20e7-4f6b-89a1-9f30fcf9f0f9` gets a random v4 UUID in G00/B00 CMNDAT (zlib from 0x130) and S00 SYDAT, as `pkp_uuid.py --set` did; the account folder is copied to `uuid-backup/<time>/` first, files are replaced by rename and put back on a failed write; a save with an interrupted commit is skipped. Host check: 0x5463c0
- `third_party/miniz`: miniz 3.0.2 (MIT) for the zlib streams
- `tools/shaders/ampere/frontend/AmpereEmit.cs` `EmitLop3Ampere`: read LOP3/ULOP3 operands after the predicate destination
  - The decoder (like nvdisasm) puts Pd before Rd (`LOP3.LUT P2, RZ, R22, 0x1, RZ, 0xc0, !PT`); the emitter read Rd first, so a, b, c and the LUT were one operand late and Pd stayed false
  - 112,542 such instructions in 32,630 corpus blocks; IADD3, LEA, IMAD and MATCH already handled predicate outputs; R2UR, FLO and LDG never carry one here
- `launcher/src-tauri/src/shaders.rs`: `TRANSLATOR_REV` 1 ⇒ 2
  - `stale_blocks` retranslates the 32,630 hashes in `rev2_blocks.txt` (SPIR-V differs before/after the fix over the full 2.0.1 corpus)
  - Blocks the game built at runtime (a few hundred, including the outline's lighting shader) are retranslated by `translate_missing`
  - Loose `.spv` files are replaced; packed `spirv.archive` gets a newer segment; the boot preload pack drops changed `results.tsv` entries
- `runtime/src/gfx/nvn/nvn_shaders.cpp`: a shader translated again has a second `results.tsv` row; the runtime indexed the first (`emplace`), so the preload pack's line-hash check passed and the old SPIR-V and resource record were used. The last row now wins (at open and in the rescan), and a pack entry whose shader gets a new row while the game runs is neither read nor copied into the next pack
- The outline is drawn by deferred lighting fs `0cd3176a1c7791d8` (G-buffer neighbour test + world-space checker), not the sky/cloud draws `8010f484`/`86aa3b21`
  - The 0.6.x #120 note credited it to the depth range default (#137); that change affects clouds and stays, but never fixed the outline

## 0.7.8 (build K: Kudu)

Press **Update** in the launcher and it does the rest. The game code is unchanged, so you do not need to **Set up again**.

This update adds a **Mods** folder for community translations and texture mods, removes the resize grip that stuttered, and makes the README available in four more languages. If a mod does not load, please open an issue and attach `pkp.log` from the game folder: the log now warns when none of the mod's files replaces a game file.

~Jerban

### Known issues
- **Linux still runs the game and the shader setup through Proton.**
- **Mods that replace files containing shaders are not supported.** Mods that only change text or textures are the intended use.
- **The translated READMEs are machine translations** and may contain mistakes. The English README is the authoritative text.
- **Use the game clock in single player only for now.**

### New

#### A folder for text and texture mods
**Before:** using a translation or texture mod meant replacing files inside the game dump, and going back to the original meant keeping a second copy.

**Now:** **Settings > Game** has a **Mods** row, saved in `pkp.ini` as `[fs] romfs_mod`. Choose a folder, and its files replace the update and base game files that have the same path. Folders are merged, and files that exist only in the mod are served too. Leave the row empty to turn mods off. Adding, swapping or removing a mod does not change the shader cache key and never asks for **Finish update**. No mod is bundled with this project: get one from its author.

**Choosing the right folder:** the folder must contain the same files and folders as the game's own `romfs`, starting at its top level. If a download has an outer folder (for example `2.0.1/romfs/...`), choose the inner `romfs` folder, the one that contains `LD_C0_R.BIN`. If the level is wrong, `pkp.log` now says that none of the mod's files replaces a game file. The README shows an example, in English and in Portuguese (Brazil).

**Still not:** mods that replace files containing shaders are unsupported and need **Set up** to be run again by hand.

#### The README in Simplified Chinese, Spanish, Arabic and Hindi
**Before:** the README was English only.

**Now:** the README and the Linux package's `README.txt` have machine translations in Simplified Chinese, Spanish, Arabic and Hindi, so more people can follow the setup and share their own translation mods. They are in `docs/i18n/`, and the Linux download includes them as `README.<language>.txt`. A pull request check keeps their structure in line with the English file, and `tools/i18n/README.md` explains how to refresh or add a language.

**Still not:** no native speaker has reviewed these translations yet. Corrections are welcome as pull requests.

#### Shader files take about a fifth of the space (prototype, not measured on a full cache yet)
**Before:** the translated shaders were ~99,000 separate files, about 26 GB, for a 12 GB game. Setup needed about 36 GB free.

**Now:** they can be packed into a few compressed files in `shader_cache/spirv.archive`. The game reads them as quickly as loose files,
so it runs the same, and nothing is translated again to switch. When the launcher finds loose shader files after a finished setup, it
asks once per start: **Free up about N GB?** (a few minutes, can be stopped and continued). **Settings > Game > Shader files** has the
same button, and **Use loose files** to go back. A new setup packs as it translates (on by default, **Packed shader files** on the
Set up page), so the loose files never pile up: a new setup now needs about 14 GB free instead of about 31 GB (36 GB was
quoted before), and the free-space check and the Set up page follow the toggle. The launcher writes the real sizes and times to
`setup.log`.
Loose shader files are on their way out: a later release stops reading them.

#### A Nearest upscaler, and the other upscalers are switched off for now (#232)
**Before:** the upscaler list offered FSR 1, FSR 3, DLSS, DLAA, NIS and XeSS. Several of them looked glitchy or no different from Off, and there was no way to keep a sharp pixel look.

**Now:** there is a new **Nearest** upscaler (sharp pixels, no smoothing), next to Off and FSR 1, in the launcher and on the F1 page. FSR 3, DLSS, DLAA, NIS and XeSS are switched off until they are reworked: if your settings use one of them, the game runs with Off instead. Their code stays in place.

If Nearest or the switched-off modes behave oddly, please open an issue.

### Fixes

#### Link Play kept old settings between sessions (#239)
**Before:** a `mode = join:<address>` left in `pkp.ini` from an earlier visit made the game visit-only, so a room you hosted could not be reached, and with a relay set the game switched Link Play off.

**Now:** the launcher puts the Link Play settings back to their defaults when it starts (Off stays Off). The Link Play page still remembers your addresses and room codes, so your last setup is one click away. The page no longer fills in this PC's own relay address while that relay is not running. The log warns when a join address is set, and internet play through a relay uses `auto` even if an old join address is left in the file. Starting the game from Steam with `--play` keeps your last saved settings.

#### Link Play now shows the address to share, and refuses your own PC's address (#245)
**Before:** the Link Play page only listed this PC's local network and VPN addresses, so a host over the internet shared the `127.0.0.1` from the relay field. On a friend's PC that address means the friend's own PC, so the friend could not find the room.

**Now:** when you host over the internet, the first row under "Share with visitors" is **Over the internet**: this PC's public address with a copy button, with the right port for your relay or for direct hosting. A visitor who types a `127.x` address is told it is their own PC. To find the public address the launcher makes one small request to a what-is-my-IP service (`api.ipify.org`, with `ipv4.icanhazip.com` as a backup), only while the page shows an internet host.

**Still not:** on a connection where the provider shares one public address between many customers, the address found is the provider's, and port forwarding cannot work there. The page cannot detect that.

#### A controller switched off and on again was not found on Linux (#226)
**Before:** on SteamOS and other Proton setups, a controller that was turned off and on again was not seen until the game was restarted.

**Now:** under Wine and Proton the game checks the four controller slots once a second and rescans the controllers when they change. The log line `rescanning gamepads` shows when it happens.

**Still not:** a controller that Proton shows only as a generic device (no controller slot) still needs a restart.

#### Photos were blank in the in-game album with HDR on (#229)
**Before:** with HDR on, photos did not show in the in-game album or main menu, while "Save to console album" still wrote a correct picture.

**Now:** the game's pictures that the CPU writes into HDR render targets are converted and uploaded instead of dropped.

If photos are still blank with HDR on, please open an issue and attach `pkp.log`.

#### DLC inside a .nup is unpacked (#236)
**Before:** the .nup unpacker skipped add-on packages, so DLC never reached the add-on folder and had to be built by hand.

**Now:** add-ons with a RomFS are unpacked into `nup_dump/aoc/<id>/romfs/`, which the launcher already finds. An earlier finished unpack gets its add-ons the next time you pick the file.

#### The bottom-right resize grip stuttered while dragging
**Before:** dragging the resize grip in the bottom right corner of the launcher stuttered.

**Now:** the grip is gone. The window edges and corners resize the window as before, without the stutter.

## 0.7.7 (build K: Klipspringer)

Press **Update** in the launcher and it does the rest. The game code is unchanged, so you do not need to **Set up again**.

Sorry for all the updates lately. I look after the Linux side, and now that Josh's one-click updater is in, I want to squash as many Linux bugs as I can and get the fixes out to you right away, instead of saving them up. Thanks a lot for your patience.

If you play on Linux, SteamOS or a Linux handheld, please keep opening issues for any bug you run into there, and always attach your logs: `pkp.log` from the game folder, and `setup.log` too if it is about Set up (the README says where they are, under "Where your saves, logs and settings are"). Without the logs I usually cannot tell what went wrong.

~Jerban

### Known issues
- **Linux still runs the game and the shader setup through Proton.**
- **Use the game clock in single player only for now.** In single player the sky and the in-game time follow it.
- **Dragging the resize grip in the bottom right corner can stutter** while the window changes size. I am looking into it.

### New

#### Set the game's date and time from the launcher (#203)
**Before:** the game's clock always followed the PC, so reaching time-gated things in the game meant changing the system clock.

**Now:** Settings > Game has a **Game clock** row: pick a date and time, or press **Real time** to follow the PC again. It is saved in `pkp.ini` as `[time] clock_offset` (minutes, up to 10 years either way) and used from the next start of the game. The clock the game reads for the player moves; the steady and network clocks stay on the PC's time, as when you set the date and time by hand on a Switch.

**Still not:** it only changes at the start of the game, not while it runs. Use it in single player only for now (the launcher says so under the row).

#### Reach the F1 menu with a controller
**Before:** the in-game settings page opened only with the F1 key and could only be used with a mouse or a touch screen, so on a handheld like the ROG Ally, or on a PC with a controller and no touch screen, you needed a keyboard.

**Now:** the "Close the game?" prompt (View and Menu together, the Switch's minus and plus) has a third choice, **Settings**: press X or tap it, and the F1 page opens. Inside the page the controller works: D-pad or left stick move, A chooses, B goes back, and plus closes it. The game gets no button presses while the page is open. The touch screen and the mouse work as before.

**Still not:** the highlight appears on your first D-pad press, not when the page opens.

### Fixes

#### The ROG Ally was not treated as a handheld
**Before:** the game only knew the Steam Deck and the Legion Go S as handhelds. On a ROG Ally it ran in the Switch's docked mode, sized its picture like a desktop PC and showed keyboard prompts by default.

**Now:** the ROG Ally and Ally X count as handhelds, like the Deck: handheld mode, a picture sized for the screen, the lighter sleep under Proton and controller prompts. The game log says `handheld: yes` with the board name. `[host] handheld = off` in `pkp.ini` turns it off again if you prefer the old behaviour.

#### The launcher was slow on Linux
**Before:** on Linux the launcher took about 0.7 seconds from a click to the next page and scrolled at about 3 frames per second, because the big background picture was drawn again, and blurred again, on every frame.

**Now:** the background is two ready-made pictures (the sharp one and a blurred one) made from the same artwork, so it looks the same. On my PC a page change takes about 55 ms and scrolling runs at about 60 frames per second. `launcher/scripts/render-art.sh` rebuilds the pictures from the SVG if the artwork changes.

#### The launcher did not fit handheld screens and was hard to use by touch
**Before:** on a 1920x1080 screen at 185 % scale (a ROG Ally on the KDE desktop) the window opened taller than the screen, so the bottom went under the taskbar. The window had no title bar, so the only way to resize it was the invisible edge of the window, which a finger cannot hit, and the window buttons were 32 pixels wide.

**Now:** on a small screen (up to 1400 pixels wide or under 800 high, as the screen counts for the launcher) the launcher starts maximized, so the taskbar stays clear. If the screen query ever fails, the launcher just opens at its normal size. On other screens it now opens a bit smaller, at 1040x600 instead of 1120x720, and it remembers the size (and, outside Wayland, the place) you leave it at. The smallest window height is 440 instead of 580, and every page works at 1038x540. Where the main pointer is a finger, buttons, rows, inputs and the window buttons are 44 to 48 pixels. There is a resize grip in the bottom right corner, and the title bar can be dragged with a finger. With a mouse nothing changes except the grip.

#### The Xbox and PlayStation d-pad, trigger and shoulder icons faded out on the move wheel (#230)
**Before:** with Xbox or PlayStation prompts, the icons for the d-pad, the triggers and shoulders, and the menu buttons are plain white art. The game's own button symbols have a dark rim, and so do our keyboard icons, but these did not, so on the pale Select Move wheel (and on white dialogue boxes) they nearly vanished. The d-pad was the worst: a white cross on a pale wheel.

**Now:** those icons get the same dark rim and dark letters as the keyboard icons. The coloured face buttons (A, B, X, Y, cross, circle and so on) are left exactly as they were.

**Still not:** I could only check the icons on their own, on a pale and a dark background, not inside the game. If any prompt still looks faint for you, send a screenshot of it.

## 0.7.6 (build K: Kinkajou)

Press **Update** in the launcher and it does the rest. The game code is unchanged, so you do not need to **Set up again**.

This one is a small fix for people who keep the game in a different folder than the launcher.

### Highlights
- **The game starts again when your game folder is not the launcher's folder.** It used to close the moment you pressed Play, with "Stopped abnormally, code 53".

### Known issues
- **Linux still runs the game and the shader setup through Proton.** A game that runs on Linux without it is something we are working on, but it is not in this release.
- **The launcher can feel slow on Linux** (page changes lag, scrolling stutters). We found the reason, it is the big background picture that gets redrawn all the time, and a fix is ready for testing. It is not in this release.
- **If Update does not work for you on Linux,** extract the new release over your old one like before and tell us what happened.
- If Link Play with pokopia-native players does not work for you, please tell us what you find.

### Fixes

#### The game closes right away with exit code 53 when the game folder is not the launcher's folder (#221)
**Before:** if you picked a game folder other than the one the launcher sits in (common with a games folder of your own on Linux), Set up built `pkp.exe` there but `pkp_rt.dll` and the ASTC decoder DLLs stayed in the launcher's folder. `pkp.exe` looks for them next to itself, so it stopped before it could write a log. Proton showed that as "code 53 (0x00000035)", which tells you nothing. The same thing made the first background run quit after zero seconds.

**Now:** Play and that first background run copy the DLLs into your game folder whenever they are missing or different, so there is nothing for you to do. If `pkp_rt.dll` is missing in both places, Home tells you what is missing and offers the game folder setting, instead of letting the game stop with a number.

**Still not:** if the launcher's own folder has lost `pkp_rt.dll` and your game folder has no copy either, you need to extract the release again.

## 0.7.5 (build K: Koala)

**This is the last update you ever install by hand.** Extract it over your 0.7.0 install one more time, and from the next
release on, updating is one click: press **Update** in the launcher and it does the rest. The game code is unchanged, so no
**Set up again** is needed.

### Highlights
- **One-click updates.** No more downloading zips, no more extracting over your install. The launcher fetches the new release,
  checks it against the release's checksums, swaps its files, restarts, and starts setup by itself if the new version needs it.
  Your saves, settings, shader cache and built game are never touched, and if anything goes wrong every file is put back.
- **A brand new launcher.** One big button on Home that always does the next right thing (Choose game files, Set up, Play),
  patch notes right inside the launcher, a crash card with the report and log, and settings that save as you change them.
- **More settings without opening an ini file:** quality preset, view distance, upscaler and its quality, frame rate counter,
  mute when not focused, keep 16:9.
- **Steam Deck and Proton find Link Play hosts on the LAN** (and are found by them), with nothing to type.
- **F1 "Just under the refresh rate" no longer slows the game** on 60 and 90 Hz screens.
- **A typo in `controls.ini` no longer wipes all your controls**, and pad combos take the Switch button names (`l+r`).

### New

#### A new launcher: one button, updates in one click, patch notes inside
**Before:** five pages (Play, Game files, Link Play, Settings, About), a long settings form with a Save button and hint text, and an "Update available" banner that sent you to GitHub to download and extract the release yourself. Release notes were only on GitHub.

**Now:** four pages: Home, Link Play, What's New, Settings, in a frameless glass window. Home has one button that always does the next step (Choose game files, Set up, Continue setup, Finish update, Play) and uses the same check as Play, so it never offers Play when Play would refuse. **Update** downloads the new release, checks it against the release's checksums, replaces the launcher's files, restarts, and starts setup by itself when the new version needs it; saves, `pkp.ini`, `controls.ini`, the shader cache and the built game are never touched, and a failed update puts every file back. What's New reads the release notes in the launcher, and Home shows the newest three. A crash shows a card on Home with its report and the game log. Setup and updates keep running when you change pages.

Settings are tabs (Display, Graphics, Audio, Controls, Saves, Game, About) that save on change, with choices instead of numbers (frame rate "Match display, 60, 120, Unlimited", render resolution "Match window, 1080p..."; Custom keeps any other value). New in the launcher: quality preset (writes the whole preset like the F1 menu), view distance, upscaler and its quality, frame rate counter, mute when not focused, keep 16:9. DLSS moved to Graphics. Link Play is one switch (Off, Same network, Internet) with every field applied by itself; the `pkp.ini` keys are unchanged.

Removed: the shader cache path (it never worked), the RomFS folder (it follows the game files), the Save/Revert bar, the old Link Play protocol choice (a notice offers to switch).

**Still not:** this update itself is installed by hand (0.7.0's launcher cannot install updates); one-click updates start with the next one.

### Fixes

#### Launcher volume could be undone by pkp.ini
**Before:** a `volume` line in `pkp.ini` beat `controls.ini`, so the volume shown was not always what played.

**Now:** the launcher also writes `pkp.ini` when it already holds the line.

#### F1 "Just under the refresh rate" no longer slows the game on 60 and 90 Hz screens
**Before:** the button set the frame limit 3 fps under any refresh rate: 57 on a 60 Hz screen, 87 at 90 Hz. At 57 the game counts every frame as 1/60 s, so it ran about 5 % slow and felt sluggish; on a screen without G-Sync / FreeSync (Steam Deck) the lower rate also showed frames unevenly.

**Now:** below 100 Hz the button sets the refresh rate itself (60, 90). From 100 Hz up it still keeps a few fps under (116 at 120 Hz, 138 at 144 Hz), which is what G-Sync / FreeSync need. The button says it is for G-Sync / FreeSync screens, with a tip. Nothing changes unless you press it.

#### Link Play: Steam Deck and Proton find hosts and are found on the LAN
**Before:** Proton has no DNS-SD (`dnsapi.dll` without `DnsService*`), so a host under Proton was invisible and a visitor under Proton found nothing (`DNS-SD needs Windows 10 1809 or later ... join by address`); every visit needed the host's address. Proton also showed a wrong "this PC's network is set to Public" note.

**Now:** where DNS-SD is missing, each scan sweeps this device's own private network (up to 254 addresses per network, 512 in all, TCP port 47600, about 300 ms) and asks every host that answers for its game over the co-op link, as for a host DNS-SD found. A hosting device answers DNS-SD queries itself (a small mDNS responder on UDP 5353 with the records pokopia-native registers), so Windows players see it. Nothing to type, and nothing to change on another network. The Public-network note is not shown under Wine. Windows with DNS-SD is unchanged. `[net] lan_sweep = off` turns it off; `NXRT_COOP_NO_DNSSD=1` tries it on Windows (a diagnostic).

**Still not:** Wi-Fi that keeps devices apart (cafes, hotels, guest networks; use internet play with a relay), and a host that fell back to another port than 47600.

#### One bad line in controls.ini no longer resets every setting (#218)
**Before:** pad combos (`[quit] pad`, `[overlay] pad`) only took position names like `left_shoulder+right_shoulder`, so `btn_l+btn_r` was an error. Any bad value in a fixed key made the whole `controls.ini` invalid: factory settings loaded, and the next save wrote them over your file. It looked like the setting reset to `none` on every start.

**Now:** combos also take the Switch names of the buttons that sit in the same place on every pad, with or without `btn_`: `l r zl zr plus minus lstick rstick` (the file is written back with the position names). A bad value only resets that one key and is logged in `pkp.log`, together with unknown keys. If a file still cannot be loaded, it is copied to `controls.ini.bad` before anything can save over it.

## 0.7.0 (build K: Kangaroo)

Extract it over your 0.6.x install, then run **Set up again** once: the game code and the shader translator changed. The launcher
asks for it. Link Play builds from before this one cannot join this one, so update together.

### Highlights
- Link Play now speaks pokopia-native's protocol, so you can play with pokopia-native players on game 2.0.1.
- Text on 21:9 and 32:9 screens no longer runs together.
- House and area transitions take the same time at any frame rate, with no 60 fps hold.
- Fewer GPU freezes on NVIDIA, GPU counters that count once, and particle effects that were missing.
- Shader setup writes about 90 files instead of 361,000, which could take hours on a hard drive.
- More graphics memory for 4K, and a smaller default picture size on GPUs with 4 GB or less.

### Known issues
- If Link Play cross-play with pokopia-native does not work for you, please report what you find.

### New

#### Link Play: cross-play with pokopia-native
**Before:** Link Play only worked between builds of this port (our own relay protocol on port 39660), so nobody could play with pokopia-native players.

**Now:** the default `[net] protocol = native` speaks pokopia-native's co-op protocol (their module, vendored in `runtime/third_party/pokopia-coop`), so this port and pokopia-native builds of game 2.0.1 meet in one session. A host listens on 47600 (TCP and UDP) and is found on the LAN by itself (DNS-SD); `[net] mode = join:<IPv4>[:<port>]` joins one host by address; `[net] internet = true` with `room_code` plays over the internet, directly or through a relay on port 47610. The launcher's Link Play page was reworked for it, and `PokopiaLauncher --host` is now a port of pokopia-native's coop relay (pokopia-native's own relay works too). New `[pkp] link_story_coop` (off by default, needs `link_interact_everywhere`, which it turns on) lets visitors talk to the story characters; every player of a session needs the same setting. Between two builds of this port, a launcher relay also carries the game's packets over UDP (a small extension that stays off, and leaves the wire unchanged, when the host, the visitor or the relay does not have it).

**Lost:** Link Play builds from before this one cannot join (set `protocol = linkplay` while everyone still has an old build), the relay hosted inside the game and the broadcast discovery on port 39660 are not used with the new protocol, and on Steam Deck / Proton the host may not be found by itself (use Visit by address). Details: `docs/multiplayer/README.md`; the README guide and the launcher's firewall hint name the new ports. Based on pokopia-native (MIT) `windows/src/coop` and `windows/tools/coop-relay` @4a72d23.

#### Shaders translated while the game runs are used without a restart
**Before:** a shader the game builds while it runs (the romfs cache cannot have it) stayed missing until the next start, even once it
was translated: what used it was not drawn for the rest of the session.

**Now:** the runtime checks `results.tsv` about once a second. When rows are added, the shaders they translate are loaded at the next
bind of the programs that lacked them. Something that translates `[nvn] dump_missing` while the game runs fills them in mid-session.
`[nvn] shader_rescan_ms` sets how often (0 = off). Thanks to @Jeffrharr.

#### Controller shortcut for the F1 menu
**Before:** the F1 menu opened only from the keyboard, so a handheld with a touch screen had no way to open it from the controller.

**Now:** `[overlay] pad` in `controls.ini` takes a button combination (like `[quit] pad`, for example `left_shoulder+right_shoulder`) that opens and closes the F1 menu. It is off (`none`) by default. A combination equal to the quit one is ignored with a log warning. Based on pokopia-native (MIT) `windows/src/nxrt/front/menu.c` @e5d9ac6.

#### Small GPUs: the default picture size stops at 2560x1440
**Before:** on a 4K screen the picture size you had not set followed the screen (3840x2160), which can run out of video memory on a GPU with 4 GB or less. Possibly related to #164 (hang at 4K while Ditto loads).

**Now:** on a GPU with 4 GB of video memory or less, a picture size you have not set is kept to 2560x1440. A set `render_width` / `render_height` or `render_scale` is never changed. `[display] vram_cap = 0` brings back the old behaviour. Based on pokopia-native (MIT) `windows/src/nxrt/vk/vk_dev.c` @e5d9ac6.

### Fixes

#### 4K: more graphics memory for the first world load (#119, #164)
**What was wrong:** at a 3840x2160 render size the game could stay on the Ditto jumping loading screen for good, or crash just after
the first world frame. The game's graphics pools were sized from what start-up uses, not what the first world load needs.

**Now:** both pools grow above 1080p (at 4K: 333 MiB more general memory and 595 MiB more for render targets). 1080p and below are
unchanged. `pkp.log`, the freeze dump and the crash report show how full each pool gets, so a report can tell which one ran out.

#### The shader warm-up no longer gives up after one empty run
**What was wrong:** the hidden first run collects the blocks the game builds as it starts. When it collected none, an empty
`shader_cache/runtime/manifest.tsv` was written, so the warm-up never ran again.

**Now:** an empty run does not mark the warm-up as done; the next Play or Set up tries again, up to two more times. Thanks to @jerbanpls.

#### Particle effects whose count the GPU works out are drawn
**What was wrong:** some effects (for example the one in the intro cave after you choose your look) were not drawn. The game counts
their quads on the GPU and draws them indirectly; the runtime converted quad draws on the CPU, could not read a count the GPU had
not written yet, and skipped them.

**Now:** a small compute shader turns those counts into an ordinary indexed draw on the GPU, at most 65536 quads and instances.
`[nvn] indirect_quads_gpu = false` brings back the old behaviour. Based on pokopia-native 1.2.6.

#### Shaders: GPU counters no longer count twice
**What was wrong:** the shader translator ran an atomic instruction twice when the shader read its result once: every counter a
compute shader keeps with an atomic add (64 of the game's shaders, 115 adds) added its amount twice, and the helper that hands the
result to the other lanes of a warp ran twice. On every GPU.

**Now:** each atomic runs once, as on the console (checked against the game's own instructions: 154 atomics in those shaders, 154
in the new translation). Run **Set up again** once after updating: the launcher asks for it, and it translates only the shaders
with atomics again (the others are kept).

#### NVIDIA: fewer GPU freezes in compute shaders
**What was wrong:** on NVIDIA GPUs (RTX 50 confirmed in pokopia-native, an RTX 3050 Laptop inferred) a compute shader that counts
with one atomic add per warp (`ballot(true)`, the highest lane adds for all) after some of its lanes have returned can stop the GPU
for good: the game freezes and Windows resets the driver ("device lost").

**Now:** on NVIDIA those counters are made per lane: every lane adds its own share and takes a slot of its own, so the warp never
waits on the lanes that returned. 30 of the game's shaders (65 counters, 132 ballots) are changed; shaders with a counter the
runtime cannot follow whole are left as translated. `[nvn] ballot_fix = auto` (NVIDIA only, the default), `on` (any GPU),
`off`. Approach from pokopia-native (MIT) `windows/docs/ballot-hang.md`, `windows/src/nxrt/vk/vk_shader.c` @4a72d23.

#### Wide screens: text no longer squeezed (#176)
**What was wrong:** on displays wider than 16:9 (21:9, 32:9) the letters in text boxes ran into each other and words merged
("Ver. 2.0.1" read as "Ver201"), and fitted text such as button labels was squashed.

**Now:** text has the same letter spacing as at 16:9. 16:9 and taller shapes are unchanged.

#### Area transitions: the iris no longer crawls or freezes above 120 fps (#144)
**What was wrong:** above about 120 fps the house-shaped wipe that closes and opens when you enter or leave a house or area
could crawl for a minute or look frozen, both on the way in and on the way out.

**Now:** the transition takes the same time as at 60 fps at any frame rate. This replaces 0.6.1's 60 fps hold: cutscenes, dialogue
and menus follow your frame limit again, and `[pkp] transition_hold` is gone.

#### Shader setup: the scan writes a few large files instead of 361,000 small ones (#194)
**What was wrong:** the shader scan wrote every shader block as its own file: about 361,000 files and 9 GB in
`shader_cache\corpus`, almost all of them from one 2.4 GB game file that is always the last one scanned. On a hard drive, and
with the virus scanner checking each new file, that can take hours, and the cleanup at the end of setup deleted them all again.
setup.log showed only the count, not which file the scan was on.

**Now:** the scan writes the blocks into a few large pack files (about 90 files, 5 GB), and only the part of each one the
translator reads. On Linux the whole scan takes 38 s instead of about 2.5 minutes. setup.log names each game file as it
finishes and, after a minute without progress, the files still being scanned. The scan uses at most four workers, and the
largest files go first. A shader corpus extracted by an older version is still translated as before. Thanks to @jerbanpls.

### Since 0.6.0
Already in 0.6.1 and 0.6.2, with details in their sections below:
- 0.6.1: a native Linux launcher and download (#52), Steam's keyboard for name entry, `PokopiaLauncher --play`, correct shader results
  on AMD GPUs and the Steam Deck, handheld mode on handhelds, the in-game clock after sleep, and the Proton "graphics device was
  lost" fix (#169).
- 0.6.2: DLC support, and a smaller Linux download that starts on more systems.

## 0.6.2 (build J: Jackrabbit)

Extract it over your 0.6.1 install: the game code is unchanged, so no **Set up again** is needed.

### New

#### DLC (add-on content)
The game can now see the add-on content you own. Put each add-on, already decrypted, in the dump folder as
`aoc/<add-on id>/romfs/` (for the Bubbly Basin key: `aoc/0400A3A01EE4D001/romfs/`). The launcher finds it, shows it on the
Game files page and sets `[fs] aoc_dir` in `pkp.ini`, and the game lists the add-on and opens its data. Like the game itself,
the add-ons must come from your own dump: the download contains none.

### Fixes

#### The Linux download starts on more systems, and is smaller
**What was wrong:** the Linux launcher needed glibc 2.39, so it did not start on Ubuntu 22.04, Debian 12, Mint 21 or Pop!_OS 22.04,
and its AppImage needed FUSE 2, which Ubuntu 24.04 and others no longer install. The download also carried the Windows-only
Python, Zig and start script (about 330 MB unpacked), and its README.txt described Windows.

**Now:** the launcher needs glibc 2.35 and no FUSE 2, the Windows-only files are left out, and README.txt explains the Linux steps.

### For developers
- `.github/workflows/launcher-checks.yml`: on pull requests and pushes to main that touch `launcher/` or `game-build/release_b/`,
  builds the launcher front end, runs `cargo check` (native and `x86_64-pc-windows-gnu`) and `cargo clippy`, and checks
  `pkp_build.py` compiles, with the cargo registry and target cached.

## 0.6.1 (build J: Jackal)

Hotfix for 0.6.0. Extract it over your 0.6.0 install: the game code is unchanged, so no **Set up again** is needed.

### New

#### The launcher runs on Linux (#52)
The launcher builds as a native Linux program (and as an AppImage), so it no longer needs Microsoft's WebView2, which Proton
does not have. On Linux it fetches its build tools on the first Set up, compiles pkp.exe there, and runs the shader translator
and the game through Steam's Proton (in the Steam Linux Runtime, as Steam starts games), umu-run or Wine. Set up, Play, Link
Play and the save import work there; see `launcher/README.md`. Thanks to @jerbanpls.

**Download for Linux:** `PokopiaRecomp-0.6.1-linux-x64.tar.gz` is the same package with `PokopiaLauncher.AppImage` in place of
the Windows launcher. Install Proton Experimental in Steam, extract it anywhere and run the AppImage; see "Linux, SteamOS and the
Steam Deck" in the README.

#### Name entry with Steam's keyboard (Steam Deck, SteamOS)
When the game asks for a name under Proton on a Steam Deck, on SteamOS or in Steam's gamepad UI, Steam's own on-screen keyboard
comes up with the question, and its Enter answers. Press any button to get the in-game keys instead; Steam + X brings Steam's back.
`[applet] steam_keyboard = off` turns it off. Thanks to @tinfoil-chewer.

#### Start the game straight from a shortcut
`PokopiaLauncher --play` starts the game the way the Play button does, without the launcher window, and waits for it: for a
Steam shortcut, Sunshine/Moonlight streaming or a desktop icon.

### Bug fixes

#### AMD GPUs (Steam Deck included): shaders computed wrong results
**What was wrong:** the game's shaders were written for NVIDIA's 32-wide subgroups; AMD runs 64 wide by default, so shaders that
shuffle, ballot or vote across a subgroup gave wrong results. Depth views in a 24-bit format were refused on AMD (it has no 24-bit
depth), so fog and soft particles that read the depth buffer covered the ground in white on the Steam Deck.

**Now:** game pipelines ask for 32-wide subgroups where the driver allows it (`[nvn] subgroup32`), and those depth views read the
32-bit depth the image really has. NVIDIA is unchanged. Thanks to @tinfoil-chewer.

#### Handhelds run in handheld mode
On a Steam Deck or Legion Go S the game is now told it is in handheld mode, as on the console's own screen, and picks its own
lighter graphics settings: a Legion Go S went from 50-57 to 62-69 fps. Under Proton on a handheld, the game's very short waits
for the GPU now sleep instead of spinning a CPU core, which leaves more power to the GPU (Steam Deck LCD: 52 to 54 fps, 1% lows
~27 to ~30). `[am] operation_mode = docked` and `[kernel] short_sleep = yield` bring the old behaviour back. Windows desktops are
unchanged. Thanks to @tinfoil-chewer.

#### Leaving an area above 60 fps: the Poké Ball wipe crawled or seemed stuck (#144)
**What was wrong:** 0.6.0's fix was not enough. Above 60 fps the wipe that opens after leaving a house crawled: about 15 seconds at
180 fps, and at higher frame rates it looked stuck until the frame limit was set to 60. A part of the transition breaks on the shorter
frames the game gets above 60 fps.

**Now:** whenever you can't move the camera (an area transition and its load, but also cutscenes, dialogue and menus) the game is held
at 60 fps, as on the console; back in free play your frame limit applies again. `[pkp] transition_hold = false` turns it off.

#### "The graphics device was lost" a few seconds after launch under Proton (#169)
**What was wrong:** on SteamOS / Proton (Steam Deck, Legion Go S) the game showed a black screen with the menu music playing, then
closed with "The graphics device was lost", unless pkp.ini had `tex_write_watch = false`. Every Proton version failed the same way.

**Now:** the game starts without the ini line. `[nvn] tex_write_watch` is `auto` (off under Proton, on under Windows).

#### The in-game clock fell behind after the handheld slept (#172)
**What was wrong:** put a Steam Deck or Legion Go S to sleep with the game open, wake it, and the in-game clock was behind by the
whole nap until you restarted the game: under Proton the counter the game's clock runs on stops during sleep.

**Now:** a few seconds after waking, the in-game clock catches up with the device's clock. `[time] resync = false` turns it off.
Thanks to @Exon0t for finding and measuring this on a Legion Go S.

## 0.6.0 (build J: Jaguar)

### Highlights
- The 3D world is drawn in the right place again below 100% scene size on Steam Deck, Legion Go S, 1440p and ultrawide screens.
- No more getting stuck on the Poké Ball wipe when you leave an area above 60 fps. Frame limits above 60 and unlimited work again.
- Lamp lights no longer show a ghost copy at 16:10 or 1440p.
- The in-game clock shows your real time.
- New one-click presets: Steam Deck, Legion Go S, Laptop and Desktop PC.
- Steam Deck: smoother play, plus a batch of anti-stutter changes.

### Bug fixes

#### The world was drawn in the wrong spot below 100% scene size (#163)
**What was wrong:** with Scene size under 100% (FSR 3 / DLSS quality presets and dynamic resolution count too) on any screen that is
not exactly 1920x1080, the 3D world went into the wrong part of the screen. On 16:10 (Steam Deck, Legion Go S) the player and objects
stood at the wrong height, floors such as the planks by the Pokémon Center were missing and a blank band sat under the picture. At
1440p and wider, only a corner of the screen showed the world.

**Now:** the world fills the frame (16:10 and 1440p, scene size 75%).

#### Stuck on the Poké Ball wipe when leaving an area (#144)
**What was wrong:** above 60 fps (or unlimited), leaving an area could leave the Poké Ball wipe half open with your controls locked.
**Now:** fixed. The stopgap that held everyone to 60 fps is gone, so frame limits above 60 and unlimited work normally again.

#### Ghost lights at 16:10, 1440p and other sizes (#44)
**What was wrong:** lamp light and other glows showed up shifted, or as a second copy, whenever the picture was not 1920x1080.
**Now:** lighting follows your render size.

#### Wrong time of day in the game (#120)
**What was wrong:** the in-game clock was off by however long your PC or handheld had been switched on (a whole day ahead on a
handheld left on for days).
**Now:** the game shows your real local time. Times saved by older versions count as coming from another clock, like a console whose
clock was reset.

#### Missing placement outline (#120)
The dashed outline when you place an item shows up again.

#### Crash on Pokédex pictures (NVIDIA) (#121)
Some Pokédex pictures crashed the game with a "GPU lost" error. Fixed.

#### Freeze at start with an incomplete game dump (#128)
Some update dumps are missing the base game's files, and the game froze at start ("exited with code 4"). If your dump also has the base
game, the launcher now combines the two for you; otherwise it tells you which files are missing and what to do.

#### Silent crash when Windows' page file is off (error 1455)
The game now tells you what is wrong and how to turn the page file back on, instead of closing without a word.

#### D-pad prompts all looked the same (#95)
With Xbox or PlayStation icons, "press up" and "press left" showed the same picture. Each direction now lights up its own arm.

#### Smaller fixes
- Setup checks that it can write to your game folder (Windows "Controlled folder access" and some antivirus block it) and says so
  plainly instead of failing halfway.
- A rare GPU hang caused by garbage draw data is prevented.
- If the GPU is lost anyway, the log records what it was doing, which makes bug reports much more useful.

### New

#### Device presets (F1 > Graphics)
One pick sets view distance, texture sharpness, render scale and frame limit:

| Preset | Render scale | Frame limit |
|---|---|---|
| Steam Deck | 75% | 40 fps |
| Legion Go S | 80% | 60 fps |
| Laptop | 75% | 60 fps |
| Desktop PC | auto | your screen's refresh rate |

Change any value and it becomes Custom. 40 and 45 fps are now in the frame limit list.

#### Scene size now works (#116)
The Scene size slider used to do nothing. Now FSR 1, NIS, FSR 3 and DLSS can draw the world smaller and scale it up, and the F1 page
tells you what is in effect.

#### F1 shows your real render resolution (#117)
The Display page shows the size actually being drawn, and what scales it up to your screen.

#### Link Play: Interact everywhere (experimental)
Outside Palette Town the game puts every Link Play player in Spectator Mode. A new toggle on the launcher's Link Play page lets you
interact in the story areas too. **Everyone in the room needs the same setting**: a player whose setting differs is refused with a
message. Off by default.

#### Steam Deck: smoother play
Shaders are no longer rebuilt in the background while you play. On a Steam Deck's 4 cores that work fought with the game itself.

#### Steam Deck and Linux: less stutter (not yet measured on a Deck)
- No more hitch about once a second: reading a picture back from the GPU no longer pauses the game.
- Something new on screen no longer freezes the game while its shader builds: it pops in a frame or two later instead. This is on for
  every platform now.
- The game's memory is handed to the GPU in a cheaper way, and each frame is sent to the GPU in one go instead of two.

These should help, but we haven't measured them on a Deck yet. Tell us how it plays.

#### Steam Deck: "Cap FPS to 30" box (F1 > Display)
On the 90 Hz Deck OLED, a 40 fps limit runs at an even 30, because frame rates that divide the refresh rate look smoother. Pick 80 Hz in
Steam's Performance menu if you want a real 40. Ticked by default only on a Steam Deck; untick it to keep your limit as set.

### Known issues
- The Steam Deck changes under "less stutter" are not measured on a Deck yet.
- On some Windows PCs the slowest frames may be a little less smooth than in 0.5.0. The average frame rate is unchanged.
- Some players crash at 4K in the Ditto jumping scene (#119). Not fixed yet.
- The launcher doesn't start under Proton / Linux (it needs WebView2), so handheld players don't see the update banner.

### For tinkerers: switching things back
Every change above can be undone in `pkp.ini` (details in CONFIG.md): `[pkp] scene_viewport_fix = 0`, `[pkp] target_fix = off`,
`[nvn] gpl_upgrade = whole`, `[nvn] readback_async = false`, `[nvn] gpl_gate = time`, `[nvn] async_whole = false`, `[nvn] async_compute = false`,
`[nvn] compute_prewarm = false`, `[nvn] pool_copy = off`, `[nvn] one_submit = off`, `[display] fps_snap = false`. New:
`[pkp] link_interact_everywhere`, `[gfx] gpu_checkpoints`, `[fs] romfs_base`, `[fs] romfs_check`.

## 0.5.0 (build I: Iguana)

### What's new
- Multiplayer (Link Play) is part of every release now. A new Link Play page in the launcher: play alone, host on this PC or visit
  a friend. Hosting shows the address to give out (LAN, Tailscale or ZeroTier) with Copy buttons, warns when Windows treats your
  network as Public, and gives you a room code. In the game, pick Local connection. Each player needs their own save: a copied save
  can't join its original.
- Room codes: a room hosted with a code is hidden from everyone else and only players with the code can join. Rooms without a code
  work as before on a LAN. Every player and the host need this version (older ones are refused with a message).
- Game traffic can travel over UDP to the relay, so a lossy internet link no longer stalls everything. TCP stays as the fallback
  (`[net] udp = off` to always use TCP).
- Steam Deck, Legion Go S and other 16:10 screens: with `aspect = auto` the picture fills the whole screen with the taller view, and
  objects and players at the right height (#73). Not yet verified in the world. `taller_fov = false` under `[pkp]` goes back to bars.
- Import your save from Eden or Ryujinx (or any folder) on the Settings page. Your current save is moved to a dated backup first.
- Set up: a setup log (Open setup log button), checks before it starts (free disk space, Vulkan, memory, a warning for Intel
  13th/14th gen desktop CPUs), and a "fewer cores" option that keeps the PC usable while it builds.
- "Set up again" is much faster after an update: compiled game code is kept and reused (up to 3 GB, `--cache-mb`).
- .nup dumps: checked against their checksums, the version is checked before the big unpack, free space is checked, and the unpack
  can be cancelled.
- Clearer dump errors: files copied over FTP in text mode, an older game version or a base-only dump now tell you what to do.
- Both sticks reach full speed by default (outer zone 0.92, new `right_stick_max`). Saved settings keep their value.
- A way out of the game besides Alt+F4 (#70): hold Select and Start (View and Menu on an Xbox pad) together, or press "Quit game" on
  the F1 page, and confirm with A.
- Steam Deck, Legion Go S and other Linux / SteamOS players: drawing work is handed to the graphics card from a separate thread by
  default (`[nvn] submit_thread = auto`); about 26 -> 53 fps on a Steam Deck (measured by a player).

### Fixes
- Saves: if a save can't be written (full disk, a file locked by OneDrive or antivirus), the last good save is kept and a notice
  says so. Before, a half-written save could replace it.
- Sound comes back after unplugging headphones, a Bluetooth reconnect or the monitor turning off, instead of staying silent.
- Windows 11 no longer slows the game down when its window is behind other windows or on a second monitor.
- Graphics: particles, light culling and a volume effect get their real buffer sizes; depth-of-field blur and lens flares get the
  right instance; volumetric fog sees its real size; shadow tests on default textures match the console.
- A rare hang in the game's waiting loops is prevented (needs Set up again).
- Safer on GPUs that lack some formats (blending, filtering and vertex formats are checked; signed 10-bit textures fall back).
- Faster pipeline lookups (less CPU per frame).

### For developers
- New opt-in keys, all off: `[sched] precise_sleep`, `[sched] precise_waits`, `[sched] yield_backoff`, `[diag] yield_census`,
  `[nvn] shader_key_check`, `[nvn] reduced_precision`. On by default: `[sched] power_throttling`, `[nvn] pipe_last`. See CONFIG.md.
- Releases run the launcher tests first and ship SHA256SUMS.txt; pull requests are scanned for private text.
- Uses code and ideas from pokopia-native (MIT), credited in CREDITS.md.

## 0.4.1 (build H: Hedgehog, hotfix)

### Fixes
- Steam Deck, Legion Go S and other 16:10 screens: trees, rocks, stairs, players and Pokémon are drawn at the right height again. They
  used to float above the screen centre and sink into the ground below it, and only settled when you walked up to them. With
  `aspect = auto` the picture now stays 16:9 with thin bars on screens taller than 16:9 (#73). `aspect = 16:10` still gives the full
  taller view, with the old placement problem, until it is fixed properly.

## 0.4.0 (build H: Hedgehog)

### What's new
- Much faster on the Legion Go S: in the world at its native 1920x1200, with no upscaler, about 55 -> 75 fps (+36%), and the slowest
  frames are smoother (1% low 31 -> 38 fps). Measured on this release build, three before/after pairs.
- Windows PCs gain too: the game needs about 20% less CPU time per frame to hand its drawing work to the graphics card (RX 9060 XT:
  6.9 -> 5.5 ms per frame).
- Each speed-up has its own setting in `pkp.ini` (see the developer notes below), so a problem on one machine can be switched off
  without losing the rest.
- The launcher shows an "Update available" banner with a "What's new" link when a newer release is out. One request at start,
  silent when offline, nothing is downloaded.

### Fixes
- Render resolutions of about 3800x2140 and up (4K) no longer crash at start: the game's graphics memory now grows with the render
  size. 1920x1080 and below are unchanged. Very large sizes (above about 5300x2990) are lowered to the largest that fits, with a
  warning in the log. Setup builds pkp.exe again once (new hook sites).
- Xbox and PlayStation button prompts now show in the game when picked in the settings or by "auto" (before, Switch prompts stayed).
- When the graphics driver loses the GPU, the game shows one clear error and closes, instead of hanging and filling the log.
- Timed waits inside the game no longer end too early (they assumed the wrong console clock rate).

### Known issues
- The launcher does not start under Proton/Linux: it needs Microsoft's WebView2, which Proton does not include. So Steam Deck and
  Legion Go S players do not see the update banner.
- Legion Go S: about 75 fps at native, not yet a steady 90.

---

### Release notes for developers

Base: fork 0.3.0 (`9edefb8`).

- **#59** (fixes #49) `pkp_gfx_heap.cpp`: mid hooks at `0x710112f008` (pool config: general `sp+0x40`, render targets `sp+0xa0`) and `0x710112fb08`
  (graphics arena `[x3+0x48]`), registered only for game targets above 1920x1080. General pool +28 B per extra pixel, render-target
  pool only past 3500 MiB (937.6 MiB + 160 B/pixel + 1 GiB), arena by the sum; pools capped under 4 GiB, larger sizes clamped.
  `main.sites` changed: installs re-lift.
- **#40** pkp prompts: the UI atlas glyph patch (`pkp_kbm.cpp`) now covers the Xbox and PlayStation styles, not only keyboard
  (fixes #39).
- **#50** Kernel wait deadlines use the registered console tick frequency instead of 19.2 MHz; regression test with 10 ms hook slicing.
- **#51** The first `VK_ERROR_DEVICE_LOST` is latched at the Vulkan device layer: replay, acquire, submit and present stop, NVN serials
  complete on the CPU so game threads leave GPU waits, one error on the main thread, then exit.
- **#42**, **#43**, **#56** README: multiplayer work-in-progress note, new layout, link to the multiplayer tracking issue.
- **#71** Performance overhaul (runtime only; no re-lift). Each item names its ini key and default:
  - Config lookups cached per thread (`[config] cache`, on). Idea from ultratears (GPL-3.0).
  - Self-sampling draws use `VK_EXT_attachment_feedback_loop_layout` instead of a copy of the attachment (`[nvn] feedback_loop`,
    auto). Legion Go S: +9% on its own.
  - Guest memory is imported into Vulkan lazily in 16 MiB pieces when first bound under Wine (`[nvn] pool_lazy`, auto = Wine only).
  - Descriptor sets are reused when the bindings did not change (`[nvn] set_reuse`, on).
  - Fewer Vulkan calls per draw: dynamic state and bindings are only set when they change (`[nvn] bind_coalesce`, on); texture
    handles are resolved once per draw (`[nvn] resolve_once`, on) with a pool lookup cache (`[nvn] pool_cache`, on). PC: dynamic-state
    calls 1542 -> 384 per frame, resolves 4.2 -> 1.0 per draw.
  - Cheaper per replay call setup: write-watch polls read a summary bitmap (`[nvn] setup_coalesce`, on). PC: 1.5 -> 0.24 ms per frame.
  - The GPU wait before the input sample lets the GPU hold two frames instead of one (`[latency] gpu_frames_ahead`, 2; 1 = the old
    wait, one frame less latency).
  - A submit thread for NVN submissions and presents (`[nvn] submit_thread`, off: no gain at the frame rate on the Legion Go S).
  - Profiling: `[prof] gpu_passes`, `replay_fine`, `render_split` and the `ftrace_report.py` / `gpu_pass_report.py` scripts (all off).
  - Legion Go S numbers: SteamOS, Proton 11, in the world at native 1920x1200 on a time-based walk, three ABAB pairs on the release
    build: 54.0 / 55.3 / 56.9 -> 74.5 / 76.1 / 75.0 fps (mean 55.4 -> 75.2, +36%), 1% low 31.3 -> 37.5, 0 error lines.
- **#72** `launcher/src-tauri/src/update.rs`: one GET of `/repos/featureJosh/pokopia-recomp/releases/latest` at start (ureq 2 +
  rustls, 5 s timeout), semver compare against the launcher's own version; untagged builds (0.0.0, 0.1.1) skip it. The banner's link
  opens only `https://github.com/` URLs.

## 0.3.0 (build G: Gecko)

### What's new
- Name entry has an on-screen keyboard drawn in the game. It works with a controller, mouse or keyboard, including in Steam's Game Mode
  on handhelds, where the old Windows popup lost keys or never got focus.
- Faster start: after the first launch, the game packs its boot shaders into one file (about 285 MB) and reads that instead of
  thousands of small files. The first launch writes it in the background.
- Steam Deck and Legion Go S are detected. Button prompts stay as controller icons when you use the trackpad or touchscreen (the
  title screen used to show Enter for A), and automatic render scale follows the screen instead of rendering at 1080p or more.
- Setup works when the Windows user folder has accented or other non-English characters in its name (it failed after compiling).

### Known issues
- The launcher does not start under Proton/Linux: it needs Microsoft's WebView2, which Proton does not include.
- Legion Go S: 60 fps, not yet a steady 90.

---

### Release notes for developers

Base: fork 0.2.0 (`98e50b8`).

- **#31** swkbd: ImGui on-screen keyboard instead of the Win32 prompt (fork #29). The game blocks its window thread while the applet
  is open: kernel waits on that thread run a hook in 10 ms slices and the last frame is presented again with the overlay on top.
- **#32** `pkp_build.py` writes the `zig ar` response files as UTF-8 (orezraey, upstream drakolordx7/pokopia-recomp#6).
- **#33** `host_is_handheld()`: `SteamDeck=1`, SMBIOS product Jupiter/Galileo or family `Legion Go S*` (registry BIOS key, else
  Wine's `Z:\sys\class\dmi`); `[host] handheld`. Render scale auto = window size; prompt style auto keeps pad prompts on mouse
  input (based on tinfoil-chewer/pokopia-recomp-linux 8b150a4).
- **#34** `<shader_cache>.preload.pack`: the order file's SPIR-V in one zstd file (1096 -> 284 MiB), written in the background after
  a boot preload, read front to back on later boots; stale entries by results.tsv line hash; `[nvn] shader_pack`. zstd 1.5.7 pinned.
  Go S: written in 20 s, preload 2.6 -> 1.7 s (tinfoil-chewer/pokopia-recomp-linux c7a15e4).

## 0.2.0 (build F: Fennec fox)

### What's new

Builds A to F were the test builds that led to 0.2.0.

**Build A: the starting point**
- Everything from the original 0.1.1 release, plus a long list of small fixes to setup, the launcher and stability.
- Colours look right now. Before, the game looked washed out and too bright (the Pokédex was orange instead of red, Ditto was pale pink).
- AMD graphics cards no longer show black or white blocks instead of textures.
- First-time setup no longer runs out of memory while it builds the game, and a step that fails is retried.
- The launcher checks shaders before every start and warms them up, so there is less stutter the first time you see something new.

**Build B: names and menus**
- Typing your name works: before, only the first letter was kept ("Ash" became "A").
- The mouse no longer makes the menu selection jump around. Click to pick what is selected.

**Build C: Steam Deck and Linux picture fix**
- On Steam Deck, Legion Go and other Linux/Proton devices, the game showed a black screen. It now draws the game.
- The sky and backgrounds are no longer black, and there are no more bright white halos around things on those devices.

**Build D: tidier game files**
- The game files moved into the folder layout the launcher expects.

**Build E: one game file, smoother on handhelds**
- The launcher can use the game as a single `.nup` package file instead of a folder of extracted files.
- Name entry: the game accepts your name and characters say it correctly.
- New controller setting: "stick max" (F1 > Controls > Gamepad) for sticks that never reach their edge.
- First speed-up for handheld PCs on Linux/Proton.

**Build F: much faster on Legion Go S and Steam Deck class devices**
- In the game world on a Legion Go S, about 35 fps before, about 55-60 fps now.
- Windows PCs are not affected by this change.

**Also in 0.2.0**
- Playing above 1080p (for example 1440p): dark outlines of characters drawn in the wrong place, and blotchy shadows, are fixed.
- Updating: when a new version changes the game code, the launcher asks you to run Set up again (once) so the new code is built.
  Every existing install rebuilds once after this update.
- Steam Deck / Legion Go S: for more fps, lower the render scale or turn on FSR 3 in F1 > Graphics (about 70-80 fps on a
  Legion Go S at 67%).

### Known issues
- Legion Go S: 60 fps, not yet a steady 90 (being worked on).
- First setup still takes a while: about 7 minutes to build the game, then 1 to 2 hours for the shaders (you can pause and continue).

---

### Release notes for developers

Base: upstream `2c29515` (v0.1.1). Fork PRs on featureJosh/pokopia-recomp; issue numbers are fork issues.

- **#1** sweep: build, test, NVN, kernel, shader and launcher fixes (upstream tests compile again).
- **#2** spirv_harness with server GC (~4x faster translation). **#4** HFMA2/HADD2 `.H0_NH1` swaps lanes and negates the low one.
  The package's `spirv_harness` is built from this source; the upstream 0.1.1 binary does not match its source (34k of 99k shaders
  differ, washed-out colours, fork #8).
- **#3** the launcher translates shaders the game built at run time; shader warm-up run before Play.
- AMD depth: `D24S8` unsupported -> `D32S8` view fallback applied to views as well as images (black/white textures on RDNA).
- **#5** setup compiles within free memory and retries a failed unit alone.
- **#9 / #20** swkbd: SDK 0x140010 packed 0x530-byte config (use_utf8 at 0x3AA); `ngc:u` name check answered; text check in UTF-8;
  #16 reverted (fork #6).
- **#10** menus: pointer movement no longer drives the selection; click = A on the game's selection (fork #7).
- **#14** NVN: every vertex-stage output starts at (0,0,0,1) at module load (`spirv_init_outputs`, `[nvn] init_outputs`); RADV left
  unwritten components NaN -> black sky, NaN bloom halos (fork #13).
- **#18** heap: private memory under Wine so pools are imported as userptr (Wine sections are file-backed), whole-pool imports
  (fork #11). Under Proton keep `[nvn] tex_write_watch = false`: write-watch mprotect invalidates the userptr BOs.
- **#15 / #17** `.nup` package as the game source (launcher + runtime, stored zstd blocks).
- **#21** `[pad] stick_max`.
- **#22** `[nvn] submit_batch` (auto = `frame` under Wine): one kernel submission per frame, merged into the present's
  `vkQueueSubmit2`; early flush only when a thread waits on or polls a batched serial. amdgpu re-walks every userptr BO on each CS
  (~4-5 ms with 4.7 GB imported). Go S in game 33-39 -> 55-60 fps (fork #19; follow-up #23).
- **#25** viewports recorded by 0x17c0420 (64 callers) follow the render size: at render heights above 1080 the G-buffer AO
  composite landed at 1.333x (dark silhouettes, misplaced ground AO; upstream drakolordx7/pokopia-recomp#17).
- **#26** rebuild on update: pkp_build.py tool stamp (lifter, main.sites, sdk, pkp_rt.lib, flags; FORMAT 2) and `--check`; the
  launcher treats a stale build as not set up.
- Packaging: `game-build/scripts/make-release.ps1` + `.github/workflows/release.yml`; third-party payload from the upstream 0.1.1 zip.
