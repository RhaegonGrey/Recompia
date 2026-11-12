# The game boundary

The runtime (nxrt) is shared and knows no game. Everything one title brings is one `nxrt::game::Info` (`include/nxrt/game.h`), filled
by the game module and registered before `nxrt::init()`. Today there is one game, Pokemon Pokopia: its module is `runtime/games/pkp/`
("the game layer" below), its lifter config is `recomp/games/pkp/`, and its program is `game-build/` (`pkp.exe`).

## What shared code may know

- Horizon and the console: the OS model, services, NVN, the system tick, and the platform facts of a console generation
  (`Info::platform`).
- Mechanisms that take game data: the frame-step patcher, the hook installer, play mode, button prompts, the NVN render facts, test
  tooling. Each reads a table from `Info`; with a null table the feature is off.
- Nothing else. Addresses, function names, texture ids, shader shapes, struct layouts, item and area names and tuned thresholds of one
  game belong in the game layer. Shared code never checks which game is running (`if (id == "pkp")`).

## The interface

`game.h` holds `Info` and a one-slot registry: `set_current(const Info*)`, `current()` and `info()` (an empty `Info` when none is
registered, so the runtime runs without a game and skips what needs one). It is header-only, so programs that do not link the runtime
(the launcher's tools) use it too. Every field is optional.

| Field | What shared code does with it | Set in (game layer) |
| --- | --- | --- |
| `id`, `title`, `product`, `short_title`, `program_id`, `version` | logs, window titles, file headers, the title profile | `pkp_data.cpp` |
| `main_base`, `languages`, `platform` | where main is mapped (hook offsets are relative to it), `control.nacp` language mask, Switch 1 or 2 platform facts | `pkp_data.cpp` |
| `exe_name`, `game_ini`, `shader_pack`, `save_folder` | file names the exe, launcher and services use; saves go under `%APPDATA%\<save_folder>` (empty: `nxrt`) | `pkp_data.cpp` |
| `romfs_min_files`, `romfs_min_bytes` | start-up check that the RomFS is complete (0: not checked) | `pkp_data.cpp` |
| `title_defaults(TitleProfile&)` | the services' title profile (main.npdm and control.nacp facts); ini keys still override | `pkp_core.cpp` |
| `timestep` (`timestep::Sites`) | sites of the real-time frame step | none set for Pokopia |
| `probes` (`tt::GameProbes`) | test-tooling state readout, warp, grant, object dump, ray casts, watches | `pkp_tt_probe.cpp` (only with `NXRT_TESTTOOLS`) |
| `play_defaults` (`play::GameDefaults`) | factory `controls.ini` text and the bindings added by later revisions | `pkp_data.cpp` |
| `play` (`play::GamePlay`, `play_game.h`) | play mode's game half: input, camera and display hooks, menu pointer, status, F1 entries, a layer drawn over its picture (`overlay_layer`, `nxrt/overlay_canvas.h`); `GameHost` is what play mode gives back | `pkp_game.cpp` and the files it installs |
| `prompts` (`play::PromptGame`) | how to find the game's button icon textures | none set for Pokopia |
| `poll_sleeps`, `poll_sleep_count` | `SleepThread` call sites inside the game's polling loops that get a shorter sleep (`[sched] poll_sleeps`) | `pkp_core.cpp` |
| `poll_waits`, `poll_wait_count` | `SleepThread` call sites inside polling loops that wait for a value in guest memory, with what each loop watches; the sleep ends when the value has changed (`[kernel] poll_wait`) | `pkp_core.cpp` |
| `settings_doc` (`setting_doc::Row` list) | the keys of the game's own ini section; `CONFIG.md`'s game section and the launcher's game-ini schema are generated from it (`apps/settings_doc.cpp`, `runtime/scripts/gen_settings_doc.ps1`) | `pkp_data.cpp`; the rows are in `pkp_settings_table.cpp` |
| `render` (`game::Render`, `game_render.h`) | graphics facts read once by the NVN back end and the extras: the texture shape `[nvn] readback_defer` may defer, the camera uniform block for temporal upscaling and its jitter, the tone-map shader matches | `pkp_render_info.cpp` |

## How the game layer registers

`pkp_game.h` declares three libraries, each a superset of the one before, so each program links only what it needs:

| Library | Adds | Register function | Linked by |
| --- | --- | --- | --- |
| `nxrt_game_pkp_data` | identity, branding, factory controls, the key table (pure data) | `nxrt_register_game_pkp_data()` | the launcher's tools |
| `nxrt_game_pkp_core` | title facts, poll sleeps, test probes (links `nxrt`) | `nxrt_register_game_pkp_core()` | test programs |
| `nxrt_game_pkp` | play-mode hooks, render facts (links `nxrt_gfx`) | `nxrt_register_game_pkp()` | `pkp.exe` |

`pkp_main.cpp` calls `nxrt_register_game_pkp()` before `nxrt::init()`. `NXRT_BUILD_GAMES` adds every module listed in `NXRT_GAMES`.

## Adding a game fact

Extend the interface; never add a game check to shared code.

1. Put the fact in a table: a field on `Info`, or a struct in its own header (like `game_render.h`) that `Info` points to. Plain data
   where possible; function pointers only for behaviour. Document what a null value turns off.
2. Make the shared code read the table, once if it is hot, and treat null as "feature off".
3. Fill the table in the game layer and say in the `game.h` header comment where it lives.
4. If the fact is an ini key of the game, add a row to `settings_doc` instead of hand-writing it in `CONFIG.md`.

A second game starts with the identity (`nxrt_game_<id>_data`) and adds tables as its hooks are found, with its own
`runtime/games/<id>/`, `recomp/games/<id>/` and `tools/` entries.

## Still on the wrong side

Game knowledge that remains in shared code (see `TECH-DEBT.md`, "Still owed"):

- The native co-op backend (`third_party/pokopia-coop`) and the BSD `coop_vnet_*` routing are registered from shared code. Plan:
  `nxrt/ldn_backend.h` (`LdnBackend`, `BsdRoute`) and `game::Info::ldn_backends`.
- Streaming thresholds (`kStreamGap`, `kStreamAfter`) and the loading-detector thresholds are tuned on Pokopia. Plan: a
  `Render::loading` block.
- Pokopia's tone-map list tries the curve match first only to keep the old order.
- Test-tooling vocabulary: `tt::GameState` fields, the hash over them (`tt_core.cpp`) and the item names in `tt_script.cpp`.
- `timestep` reads `[test] clock_step_ticks` itself.
