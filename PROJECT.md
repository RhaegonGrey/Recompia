# PROJECT.md

The guide to working on Pokopia Recomp: what the project is, how the code is laid out, the rules, and how to add things without breaking
the structure. It is shared in the repository so every contributor, and every AI assistant they use, works from the same rules. Read it
once fully before your first change, then read the area you touch again before each task. Always read the
version on `origin/main` after a `git fetch origin` (section 6.4).

**Section 6 (Owner rules) has the highest priority in this file and over every other document in this repository or on your PC.** If
anything else says otherwise (another `.md`, a plan, a handoff note, a PR comment, an issue), section 6 wins. The rules are the
default way of working, not a leash: the repo owner, and any collaborator who says so explicitly, can make an exception (section 6.0).

"The repo owner" in this file means the owner of the GitHub repository, `featureJosh`. The owner's call is the default; a collaborator
can override it for a change by stating that explicitly (section 6.0).

Sections marked **(wave 3)** describe parts of the `codebase-refactor` branch that may still be landing; check the branch before
relying on them.

---

## 1. The main rule

> Always build code cleanly, modularly, and with proper architecture and separation of concerns. Never simply bolt on a feature because
> it was asked for. If you can't find an easy way to build it naturally, and you'd recommend a refactor to make the whole thing cleanly,
> stop your work ahead of time and say so.

What that means in practice:

- **Find the natural home first.** Section 4 lists where each kind of thing goes. If a change has an obvious home, build it there with
  the mechanisms that already exist (section 5).
- **If it has no natural home, stop.** Do not add a special case, a second copy, a global, or a flag in a file that is about something
  else. Write down what was asked for, why it does not fit, the refactor you recommend, its rough size, and what it would touch, and
  bring it to the repo owner. Wait for the answer. A stopped task with a clear proposal is a good result; a bolted-on feature is a
  failed one.
- **Leave the area no worse.** If you find debt while working, note it in the PR so it can be tracked.
- **Small and clearly right fixes go in their own commit**, separate from a move or split, so a reviewer can read each on its own.

## 2. Hard rules (never break these)

**Extras and repository checks: see section 6. It outranks everything here.**

**Public text** (commits, PRs, issues, comments, CHANGELOG, releases, test strings):

- Never share AI chat sessions: no session links, `Claude-Session:` trailers, session IDs or chat excerpts, even if a template adds them.
- No model names. The commit trailer is exactly `Co-Authored-By: Claude <noreply@anthropic.com>`.
- No personal names, PC host names, `.local` domains or LAN IPs. Git identity comes from your own git config; never pass `-c user.*`.
- Never write test status ("not tested", "compile only", "not confirmed"). What was not verified goes to the reviewer privately.
- Modding and reverse-engineering notes stay in a local, git-excluded `modding-docs\`, never in a public `docs/`.

**Repository:**

- Never commit game data: `/game/`, `/work/`, `*.nso`, `exefs/`, `romfs/` are git-ignored. Add files by path, never `git add -A` blindly.
- Game-specific code lives only in `recomp/games/pkp/` and `runtime/games/pkp/`. Everything else is shared (nxrecomp, nxrt) and must
  know no game: no "Pokopia", no `pkp.ini`, no game addresses, no texture ids, no game-tuned thresholds. Shared code asks the game
  through `nxrt/game.h` (`game::info()`) or an interface the game registers.
- Keep each file's line endings (many are CRLF). Use an editor that keeps them; Git Bash `sed -i` strips CR.
- Remove a worktree with `git worktree remove`, never by deleting the folder.

**Testing and the PC:**

- AI agents do not start the game, and never move the real mouse or type real keys, unless the person running them says so.
  Verification is a clean build plus careful reading.
- The runtime has no unit tests (removed on purpose). Do not add any. The launcher's Rust tests and JS checks are kept and must pass.
- Never run `cargo test` with `NXRT_FS_SAVE_DIR` set (it may point at real saves): clear it first.

**Players' settings:**

- Every new setting or rebind needs a working row in the game's own Settings screen. The F1 menu is being phased out.
- A feature that overrides a player's own setting (frame cap, resolution, aspect, quality) defaults OFF. That is not an extra.
- Extras (gameplay mods) are opt-out and default ON, always (section 6).
- An old `controls.ini` / `pkp.ini` must keep loading: a removed key is skipped silently (`retired_*`), a renamed one is migrated
  (`replaced_act`), never an error.
- Multiplayer stays compatible on the wire with older builds.

## 3. What the project is

Static recompilation of Pokémon Pokopia (Switch 2, program `0400A3A01EE4C000`, v2.0.1a = update 655360) to native Windows x64, also
played on Linux handhelds through Proton (Steam Deck, Legion Go S). Goal: good to play on every PC and handheld; fix what is broken,
make it fast, ship releases from the fork (`featureJosh/pokopia-recomp`). The fork is its own line: upstream (`drakolordx7`) gets a PR
only when the repo owner asks for that exact one. The parent project to follow for updates is `xFractals/pokopia-native` (remote
`native`; process in CONTRIBUTING.md).

| Folder | What | Game-independent? |
|---|---|---|
| `recomp/nxrecomp/` | Python AArch64 to C++ lifter | yes |
| `recomp/games/pkp/` | the game's lifter config (`config.py`, `main.sites`) | no |
| `runtime/` (except `games/`) | nxrt: C++20 runtime: kernel, services, NVN on Vulkan, play mode | yes |
| `runtime/games/pkp/` | the game layer: hooks, mods, game settings | no |
| `game-build/` | links `pkp.exe` and `pkp_rt.dll`; `release_b/` is the player-side setup | no |
| `launcher/` | Tauri 2 launcher: Rust back end, plain JS pages | no (it is this game's launcher) |
| `tools/` | shader translator (.NET 10), textures, tiling, i18n, scripts | mostly |

## 4. Architecture map: where things live

### 4.1 Runtime (`runtime/`)

Public headers are `runtime/include/nxrt/*.h`. A shared helper exists once; use it, never copy it:

| Need | Use |
|---|---|
| Log | `NXLOG_I/W/E/D(tag, ...)` from `nxrt/log.h` (all levels gated by one `NXLOG_AT` macro) |
| Trim, lower, split, number parsing | `nxrt/text.h` |
| High-resolution time | `nxrt/qpc.h` |
| A counter or timer shown in diagnostics | `GaugeHook` in `nxrt/gauge.h` |
| Winsock start-up | `ensure_winsock()` in `src/net/win_sock.h` |
| "Is the host kernel Linux?" (natively, or under Wine) | `host_is_linux()` in `nxrt/host_device.h`; ask the kernel or driver, never "is this Wine" |
| "Which machine is this?" (handheld, Steam Deck) | `host_is_handheld()`, `host_is_steam_deck()` in `nxrt/host_device.h` |
| Ini value or env override | `nxrt/config.h` (`config_get_*`; env `NXRT_<SECTION>_<KEY>` wins) |
| A fact about the game (name, log file, ini file, defaults) | `game::info()` in `nxrt/game.h` |
| Hook a guest address | `nxrt/hooks.h` (in the game layer: always through a `HookSet`) |
| Vulkan object boilerplate | `src/gfx/extras/vk_make.h` |
| FNV-1a | `src/gfx/nvn/nvn_fnv.h` (persisted hashes must not change) |
| A short message over the game's picture | `nxrt/notices.h` (drawn by the overlay; one per source) |
| A screenshot of the next presented frame, from code with no renderer | `nxrt/screenshot.h` (play mode registers the provider) |
| Which keyboard keys are held, outside the event pump | `nxrt/key_state.h` (fed by play mode; SDL scancodes) |
| A colour matrix over the final picture | `nxrt/color_matrix.h` (read by the output pass) |
| An HTTP(S) request from the host | `nxrt::http` in `src/net/http_client.h` (https only, no redirects, no cookies) |

Source tree (`runtime/src/`), one job per file; the name says the job:

- `core/` start-up, dispatch, hook engine, module loading. `kernel/` SVCs split by area (`svc_memory`, `svc_thread`, `svc_ipc`, ...),
  scheduler, sync. `mem/`, `loader/`, `eh/`, `ipc/`, `diag/`, `media/`, `timestep/`.
- `services/` one HLE service family per file group: `network_*` (bsd, dns, nifm, ssl, social), `ldn_*` (local play: LAN, relay,
  Link Play), `misc_*` (aoc, nfc, olsc, system, ...), `audio_*`, `hid*`, `fs*`, `vi_*`, `am`, `acc`, `time`.
  `*_internal.h` headers are for that family's files only.
- `net/` sockets, virtual network, relay wire format, lobby, `http_client` (the host's HTTP(S) client).
- `mods/` the Modtopia mods host (public entry points in `nxrt/mods.h`; the game supplies its folders through `game::Info::mods`):
  `modtopia_host.cpp` (start, hook points, events), `modtopia_backend.cpp` (the `MtBackend` the Modtopia core calls),
  `modtopia_input.cpp` (the mods' pads), sharing `mods_internal.h`.
- `gfx/nvn/` NVN on Vulkan. `api_*.cpp` are the guest-facing NVN entry points. The command replay is `nvn_replay.cpp` (the loop) plus
  `nvn_replay_{pass,textures,compute,bind,upscale,query,draw,trace}.cpp`, sharing `nvn_replay_internal.h`. GPU textures are
  `nvn_gpu_texture.cpp` plus `_upload`, `_readback`, `_sparse`, `_views`, `_debug`. Pipelines: `nvn_pipeline.cpp`, `nvn_gpl.cpp`.
  Present: `nvn_present*.cpp`.
- `gfx/extras/` what the game never asked for: HDR (`hdr_*`), upscalers (`fsr1_*`, `nis_*`, `temporal*` for FSR3/DLSS/XeSS),
  low latency, output pass, overlay compositing.
- `gfx/play/` play mode, the host side of playing the game:
  - settings file: `play_config.cpp` (reader), `play_config_apply.cpp` (one setter per key), `play_config_format.cpp` (writer),
    `play_config_migrate.cpp` (defaults revisions), `play_config_io.cpp` (disk); types in `play_types.h`
  - frame: `play_engine`, `play_frame`, `play_frame_cap`, `play_events`, `play_window`, `play_display`, `play_startup`, `play_state`
  - input: `pad_map`, `pad_orient`, `play_vpad`, `prompts`, `prompt_resolve`
  - F1 menu: `play_ui.cpp` (frame and tabs) plus one file per page (`play_ui_controls`, `_display`, `_graphics`, `_audio`, `_about`)
  - the boundary to the game layer: `play_game.h` (`GamePlay`, `GameHost`, `GameFrame`)
- `gfx/overlay*.cpp` on-screen overlay: HUD, diagnostics, on-screen keyboard, quit prompt (`overlay_impl.h` shared).

### 4.2 Game layer (`runtime/games/pkp/`)

One module per feature, in four folders: `core/` (the base every module builds on), `input/`, `render/`, `ui/`, plus `test/`
(test-only probes, never in a release). The pieces every module builds on:

| File | What it is |
|---|---|
| `core/pkp_feature.h` | `Feature`: what a module declares (install, remove, frame, game_frame, settings_changed, status_text) |
| `core/pkp_game.cpp` | **the feature registry**: one ordered table (`kEntries`), with a comment saying why each order constraint exists. Each row is `mod(feature, switch...)` (a gameplay mod, named with its switches) or `port(feature)` (part of running the game on PC); a compile-time check makes every mod switch belong to exactly one `mod()` row |
| `core/pkp_hook_set.h` | `HookSet`: every hook a module adds; the registry removes them all at stop |
| `core/pkp_settings.h` + `core/pkp_settings_table.h` | **every `[pkp]` ini key declared once**: type, default, range, choices, when it applies, one-line doc. Extras are the rows wrapped in `gameplay(...)` (section 6); `opt_in(...)` makes one off by default, `hidden(...)` gives it no launcher row, `under(Switch, ...)` makes a key a setting of that mod. `pkp_settings_table.cpp` has `spec()`, the doc rows and the release check |
| `core/pkp_guest.h` | the one set of guest read/write/call helpers |
| `core/pkp_layout.h` | game addresses and struct offsets, declared once |
| `core/pkp_tables.h` | the one data-table row lookup |
| `core/pkp_text.h` | the one "set a widget's text to our string" |
| `ui/pkp_key_guide.{h,cpp}` | the key guide widget: owns its hooks; features register as `Client`s |
| `ui/pkp_options.cpp` | our rows in the game's own Settings screen: the slot state machine and the game's hooks |
| `ui/pkp_options_model.{h,cpp}` | what a tab is to that screen (`TabModel`); `StaticTab` serves the row tables of `pkp_options_internal.h` |
| `ui/pkp_options_capture.{h,cpp}` | the key capture behind capture rows |
| `ui/pkp_options_mods.cpp` | the Mods tab: the Modtopia mods, through the core's own list and settings API |
| `ui/pkp_menu*.cpp` | the menu mouse pointer (scan, pick, draw, in-game) |
| `input/pkp_play.cpp` + `render/pkp_render_size`, `pkp_render_targets`, `pkp_aspect`, `pkp_wide_text`, `pkp_frame_rate`, `input/pkp_mouse_camera`, `pkp_mouse_mode` | play-mode hooks, one subsystem per file |

Features today (`kFeatures` order): save_uuid, linkplay, play, kbm (prompt icons), movie_test, menu, quality, taa, taa_probe, jump,
aim, title, sort, craft, key_guide, options. A new module goes in the folder of its kind.

### 4.3 Launcher (`launcher/`)

- Rust (`src-tauri/src/`): `main.rs` only wires things up and lists commands in `generate_handler!`. Commands live in `*_cmds.rs`
  (`settings_cmds`, `profile_cmds`, `repair_cmds`, `update_cmds`, `dump_cmds`, `relay_cmds`, `backup_cmds`, `steam_cmds`, `dlss_cmds`);
  the logic they call lives in the module of the same name without the suffix. Shared helpers: `fsutil.rs` (copy_tree, atomic write),
  `ini.rs` (the one ini reader/writer), `paths.rs`, `events.rs`, `guard.rs` (busy checks), `dialogs.rs`.
- Pages (`src/`): a page never types a command name. Every command is one function in `src/api.js`. Pure logic sits in `*-model.js`
  files (testable without a window); pages in `src/pages/`; settings rows come from `settings-schema.js`.
- `scripts/check-api.mjs` proves `api.js`, the pages and `generate_handler!` agree. It runs in CI (`launcher-checks.yml`).

### 4.4 Lifter and build

- `recomp/nxrecomp/` is the generic lifter (`driver.py` orchestrates; `lift/` instruction semantics; `module_emit.py`, `outfiles.py`
  output). The game's choices are `recomp/games/pkp/config.py` and `main.sites` (hook sites: `entryhook`, `midhook`, ...). A new hook
  site needs a line in `main.sites` and a re-lift.
- `game-build/` links the lifted objects with the runtime. `release_b/pkp_build.py` is what a player's PC runs at setup.

## 5. How to add things (recipes)

**A game feature or mod** (`runtime/games/pkp/`):
1. New `<folder>/pkp_<name>.{h,cpp}` (`input/`, `render/` or `ui/`). Export one `const Feature k<Name>Feature`.
2. Register every hook through the `HookSet` given at install. Never call `hook_add_*` directly, never install another module.
3. Add the feature to the registry table in `core/pkp_game.cpp`: `mod(&kNameFeature, Key::NameSwitch)` for a gameplay mod, `port(...)`
   for anything that is part of running the game on PC (display, input, menus, Link Play, test tools). If its position matters, say why
   in the table's comment.
4. Game addresses and offsets go in `core/pkp_layout.h`; guest access through `core/pkp_guest.h`.
5. Its on/off switch and tunables are keys in the settings table (next recipe), with a row in the game's Settings screen. If it is a
   mod, its switch is a `gameplay(...)` row that defaults ON (section 6) and its settings the launcher should show are `under(Switch, ...)`
   rows. That is all the launcher needs: Settings > Gameplay is built from the generated schema, and `test-settings-schema.mjs` fails
   until the row has a label in `GAMEPLAY` (`launcher/src/settings-schema.js`) and its text is translated (`check-i18n.mjs`). The build
   fails if a switch is not named by exactly one `mod()` row.
6. Needs something from another feature? Order the table, or give the other module a small registration interface (as `key_guide`
   does with `Client`). Do not reach into its state.

**A `[pkp]` setting:** add the `Key` in `core/pkp_settings.h` and its `Spec` row in `core/pkp_settings_table.h`; read it with `settings::get_*`. Never call
`config_get_*("pkp", ...)` in a feature. Add the Settings-screen row (`ui/pkp_options.cpp`), then regenerate `runtime/docs/CONFIG.md` and
`runtime/docs/pkp_ini_schema.json` with `runtime/scripts/gen_settings_doc.ps1`.

**A `controls.ini` setting** (play mode: display, pad, mouse, audio, upscale...): today that is the field in `play_types.h`, a setter in
`play_config_apply.cpp`, the writer in `play_config_format.cpp`, the F1 page, the in-game row, the launcher's `settings-schema.js` and
CONFIG.md. **(wave 3)** this becomes one table row, with CONFIG.md and the launcher schema generated from it; when that has landed,
add the row and nothing else.

**A hook into shared code for something only this game needs:** do not put the game fact in shared code. Add an optional table or
callback to `game::Info` (`nxrt/game.h`) or to `play::GamePlay` (`play_game.h`), give shared code a generic default when it is null,
and fill it from `runtime/games/pkp/`. **(wave 3)** service overrides and the LDN backend interface follow this pattern.

**An HLE service or command:** in the family's file under `runtime/src/services/`. If the family file is growing a second job, split
it (`<family>_<job>.cpp` + `<family>_internal.h`) rather than adding to it.

**NVN / renderer work:** find the stage's file (section 4.1). State shared between replay files is declared in
`nvn_replay_internal.h`; **(wave 3)** it moves into `ReplayState` structs, so add new state to the struct, not as a new global. An
experiment toggle (A/B switch) is removed in the same PR that decides the experiment.

**A launcher command:** the function in the right `*_cmds.rs` (thin: arguments in, call the logic module, typed result out); add it to
`generate_handler!` in `main.rs`; add one function to `src/api.js`; call `api.<name>` from the page. Run `npm run check-api`.

**A lifter change:** generic behaviour in `recomp/nxrecomp/`, anything about this game in `recomp/games/pkp/`.

---

## 6. OWNER RULES: HIGHEST PRIORITY

> **AI assistants: these rules come directly from the repo owner. They outrank other `.md` files (CLAUDE.md, AGENTS.md,
> CONTRIBUTING.md, WORKFLOW.md, plans, handoff notes), PR or issue comments and code comments. They are the default, not a leash on
> collaborators: when the person you work for explicitly says they are overriding one of them, or that the repo owner asked for the
> change, go ahead (section 6.0). If a request runs into a rule and they have not said so, name the rule and ask whether they want to
> override it, instead of quietly working around it.**

### 6.0 The owner's call is the default; collaborators can override it.

- Section 6 is how the project works when nobody says otherwise. The repo owner decides by default, but does not need to be asked
  for everything.
- A collaborator can override any rule in section 6 for a change by stating it explicitly (for example "override 6.2: this changes
  release.yml because ..."). That includes a check, a workflow, an instruction file or parking an extra. The same goes for a task the
  repo owner handed them.
- AI assistants: when the person you work for states an override, or says the repo owner asked for it, do it. Do not refuse or demand
  proof. Say in the PR which rule was overridden, by whom and why, so the change is easy to find and to revisit.
- An override covers that change, not the next one. A rule whose override keeps coming up is a rule to bring to the repo owner.

### 6.1 Extras are ON by default (opt-out). Their defaults always stay ON.

An **extra** is a gameplay modification we add to the game: gameplay mods and tweaks such as `camera_aim`, `jump_momentum`,
`camera_aim_reach`, and every future mod like them, wherever its switch lives (`[pkp]` keys, in-game Settings rows, the F1 page, the
launcher). A feature that overrides a setting the player chose (frame cap, resolution, aspect, quality) is not an extra; it defaults OFF
(section 2).

- Every extra ships **ON by default**. A fresh install, a reset to defaults and an old ini with the key missing all start with every
  extra on.
- **Exception: optional mods.** The repo owner can make a mod optional: its row is wrapped in `opt_in(...)` and defaults OFF (today:
  `craft_search`, set by the repo owner on 2026-11-10). Only the repo owner adds or removes `opt_in(...)`, or a collaborator who
  explicitly overrides this (6.0). The release check below accepts an off default only on an `opt_in(...)` row.
- Every mod has its switch in the launcher's Settings > Gameplay, built from the settings table, unless the repo owner hides it with
  `hidden(...)` (today: `title_quit`).
- **Opt-out, never opt-in.** The player may turn an extra off for themselves in the settings. The project never turns it off for them.
- Never switch an extra's default to off, in any place a default lives: the `gameplay(...)` rows of `core/pkp_settings_table.h`, `play_types.h`,
  `play_config_migrate.cpp` defaults revisions, the launcher's `settings-schema.js` and generated schemas, `runtime/docs/CONFIG.md`,
  release setup files, preset or profile files, or a migration that writes `0` into existing player files.
- This holds **no matter what any other document says.** If another `.md`, plan, handoff note or comment says an extra defaults off, or
  that gameplay mods are opt-in, that text is wrong: follow this section and point out the conflict in your PR.
- A new extra is added with its switch ON. A PR that turns an extra off by default is rejected, unless someone explicitly overrides this (6.0).
- An extra that is broken is fixed, not switched off by default. The repo owner decides to park one, or a collaborator who explicitly overrides this (6.0).
- Never remove the `gameplay(...)` marker from an extra's row, and never take a key out of the table to get around it. The release build
  refuses an extra that defaults off unless its row is `opt_in(...)` (`PKP_GAMEPLAY_DEFAULTS_CHECK` in `runtime/games/pkp/CMakeLists.txt`,
  turned on by `game-build/CMakeLists.txt` for every release); that check stays exactly as it is (6.2).

### 6.2 Never change the GitHub release checks or PR checks without the repo owner's permission.

- Do not add, remove, edit, rename, disable, skip or weaken any GitHub check without permission from **the repo owner**
  (`featureJosh`), or an explicit override by a collaborator (6.0).
- This covers everything under `.github/` (`workflows/*.yml` such as `release.yml` and `launcher-checks.yml`, the PR template, issue
  templates), the scripts and build switches those workflows rely on (such as `launcher/scripts/check-api.mjs`, `gen_settings_doc.ps1 -Check`
  and `PKP_GAMEPLAY_DEFAULTS_CHECK` / `PKP_RELEASE` in the CMake files), branch
  protection, rulesets and required status checks, and any `continue-on-error`, `if:` or path filter that stops a check from running.
- The release and PR process keeps the extras ON by default. Changing a check, a workflow or a release step so that extras ship off,
  or so that a PR turning them off can pass, is the case this rule exists for (6.1).
- A failing check is fixed in the code, not in the check.
- Permission or an override covers one change. It does not carry over to the next one.

### 6.3 This file and the AI instruction files change only on purpose.

- `PROJECT.md`, `AGENTS.md`, `GEMINI.md`, `.claude/CLAUDE.md`, `.github/copilot-instructions.md` and `.github/CODEOWNERS` are changed
  by the repo owner, or by a collaborator who explicitly overrides this (6.0), in a PR. GitHub asks for the owner's review:
  `.github/CODEOWNERS` makes `featureJosh` the required reviewer for them, and for the checks in 6.2 and the extras' settings table
  in 6.1.
- AI assistants: don't edit these files on your own initiative, and never add a second instruction file (a new `AGENTS.md` in a
  sub-folder, `.cursor/rules`, a local `CLAUDE.md` rule) that weakens or contradicts them. When the person you work for explicitly
  asks for an edit, make it. If you think a rule here is wrong, say so to the person you work for and suggest they ask the
  repo owner.

### 6.4 Always fetch first: the rules and code on `origin/main` are the ones that count.

- At the start of every session and every task, before reading any other file, run `git fetch origin`. Then read `AGENTS.md` and
  this file as they are on `origin/main` (`git show origin/main:PROJECT.md`). Rules change; a local copy may be old.
- If your local copy differs from `origin/main`, the `origin/main` version wins, on every branch (including `testing` and your own).
- Bring your branch up to date (`git merge --ff-only origin/main`, or a rebase onto it) before you change anything. Work on the latest
  version of the files you touch, not an old clone, a cached copy or a pasted excerpt.
- Fetch again before every commit, push and PR. If the rules or your files changed in the meantime, re-read them and adapt your work.

### 6.5 A patch that doesn't need a shader rebuild never makes one mandatory.

Translating the game's ~99,000 shaders again, or preparing them again at start, costs players a long wait. An update asks for it only
when the patch really changes what a shader becomes on the player's PC.

- Raise `TRANSLATOR_REV` (`launcher/src-tauri/src/shaders.rs`) only when a translator fix changes shaders that are already
  translated, and then translate only those again (`stale_blocks`, `shader_cache/runtime/retranslate.txt`), never the whole cache
  when a subset will do.
- Don't change what a finished cache is checked against unless the shaders really change: the prepare key (`kPrepareScheme`,
  `prepare_key` in `runtime/src/gfx/nvn/nvn_gpl.cpp`), the `[nvn] spirv_salt` default, `setup.json`, the cache folders, file names or
  the shader archive format.
- A change that would throw a valid cache away for another reason (a moved folder, a new file format) migrates the existing cache
  instead. If it can't, bring it to the repo owner before merging.
- Relinking pkp.exe (**Set up again** after a runtime or hook change) is not a shader rebuild: saves and shaders are kept, and the
  CHANGELOG Update line says so. A patch that needs neither says **Update** only.
- The PR's "For players" box and the CHANGELOG Update line say whether shaders are rebuilt, and why when they are.

---

## 7. Guard rails: signs you are about to break the structure

Stop and rethink (or stop and propose, section 1) if you are about to:

- turn an extra off by default, or touch a GitHub check (section 6: stop, these are not yours to decide);
- write "Pokopia", "pkp", a game address or a game texture id outside the two `games/pkp` folders or the launcher;
- copy a helper (trim, FNV, guest read, copy_tree, ini parsing, Vulkan create-info) instead of including the one that exists;
- add a global to a file that already has an owner struct, or a `static` singleton with a hand-written reset;
- call `hook_add_*` or `config_get_*("pkp", ...)` directly in a feature module;
- read the same ini key in two places, or give one key two defaults;
- add a setting with no in-game row, or with no entry in the settings table;
- register a gameplay mod as `port(...)` in the feature registry, or give a mod no `gameplay(...)` switch, so it never reaches the launcher;
- type a Tauri command name as a string in a page, or put logic in `main.rs`;
- add a job to a file whose name describes a different job, or push a file past roughly 800 lines (split by job instead);
- add an `#ifdef`, env var or A/B toggle to keep an old path alive "just in case" (delete the old path; git is the rollback);
- leave code nothing calls, a setting nothing reads, or a doc that describes what no longer exists;
- make one module install, tick or reset another;
- add a runtime unit test, or start the game to check something.

Comment style: dense and factual, saying what the code relies on and why (bug number where one exists). No narration of the change.

## 8. Build and verify

From a PowerShell in the checkout or worktree root. `runtime\scripts\env.cmd` sets up MSVC, clang-cl (LLVM 23), CMake, Ninja, sccache.

    # runtime DLL and game exe (the usual check after any C++ change; both must exit 0 with no new warnings)
    cmd /c "call runtime\scripts\env.cmd && cmake --build game-build\build\rtdll --target pkp_rt -j 5" *> work\build-rtdll.log; $LASTEXITCODE
    cmd /c "call runtime\scripts\env.cmd && cmake --build game-build\build\exe --target pkp -j 5" *> work\build-exe.log; $LASTEXITCODE

    # first time in a new worktree: configure with absolute paths into your main checkout
    #   -DPKP_LIB_DIR=<checkout>/work/build/pkp   (shared lifted objects, read-only)
    #   -DNXRT_A2B_DIR=<checkout>/tools/textures/build/release

    # launcher (in launcher\)
    npm run check-api ; npm run build ; node scripts/test-gamepad-nav.mjs
    # launcher Rust (in launcher\src-tauri\, with $env:NXRT_FS_SAVE_DIR = $null)
    cargo check --all-targets ; cargo test

    # lifter
    python -m compileall -q recomp/nxrecomp recomp/games

    # full builds
    pwsh -File runtime\scripts\build.ps1            # runtime
    recomp\scripts\build_game.cmd pkp               # lift + compile the game (needs -DPKP_EXEFS=<your exefs dir>)
    pwsh -File game-build\scripts\build.ps1         # link pkp.exe
    cd launcher; npm install; npm run tauri build   # launcher

Before calling a change done: builds exit 0; launcher checks pass if the launcher was touched; `git ls-files --eol` shows no changed
line endings; you have re-read your own diff; no extra's default changed and no check was touched (section 6); you have the list of
what a person must check in the game.

## 9. Workflow

- `git fetch origin && git merge --ff-only origin/main` before branching and before every commit, push and PR (section 6.4).
- One branch per piece of work, off `main`. PR against the fork's `main`. Fill the template, add `## Pros` / `## Cons`.
  Screenshots only as before/after pairs for big visual bugs.
- While `codebase-refactor` is open: new non-urgent work branches from it; hotfixes go on `main` and are ported across.
- Commit subjects short and plain ("Game layer: key guide module"); the body says what and why. One logical step per commit.
- Player-visible changes get one line under `Unreleased` in CHANGELOG.md.
- Merged PRs: delete the branch and its worktree at once. No second branch for a follow-up.
- Keep your own working notes (state, branches, open bugs) in a local, git-excluded file so a cut-off session can resume.
- Perf numbers always name the device (Go S, Deck or PC). In-world tests only; a flat fps is not proof of a cap (check vsync/refresh).
- Working with AI sub-agents: one mission per fresh agent with its own worktree and a written brief; review every commit; keep, or
  purge and redo. Agents never push. Watch background agents for stalls.

## 10. Setup

| Item | Notes |
|---|---|
| Game files | your own dump, outside git (`/game/`, `/work/` are ignored). exefs hashes must match `game-build/release_b/dump_hashes.json` |
| Saves | point `NXRT_FS_SAVE_DIR` at a folder outside any play folder you copy to other devices, so copies never ship saves |
| Toolchain | VS 2022 BuildTools (MSVC 14.44), LLVM 23, CMake, Ninja, sccache, Rust, Node 24, .NET 10, Python 3.13 + uv, gh, PowerShell 7; vcpkg at `C:\vcpkg` |
| Recompiler venv | `tools\.venv` (`uv venv tools\.venv --python 3.13`, then `uv pip install --python tools\.venv -r recomp\requirements.txt`) |
| Texture tools | `tools\textures\build\release` |

## 11. Document map

| File | For |
|---|---|
| `PROJECT.md` | this guide (section 6 outranks every other document) |
| `AGENTS.md` (+ `.claude/CLAUDE.md`, `GEMINI.md`, `.github/copilot-instructions.md`) | what every AI assistant loads first: the owner rules in short, and "read PROJECT.md" |
| `.github/CODEOWNERS` | the files only the repo owner can approve |
| `runtime/docs/CONFIG.md` | every ini key and env var |
| `runtime/docs/GAME_BOUNDARY.md` | the shared/game boundary in detail |
| `CONTRIBUTING.md` | process: parent sync, releases, patches |
| `CHANGELOG.md` | player-visible changes |

## 12. Open questions

- Older game versions (pokopia-native targets update 327680): needs an older dump.
- Shader updates that never block Play: only when the repo owner says go.
- Cross-play with pokopia-native, lazy memory commit, indirect quad draws: pinned for later.
