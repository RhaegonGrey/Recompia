// SPDX-License-Identifier: GPL-3.0-or-later
// Test tooling of the runtime (S5): deterministic mode, frame boundary, per-frame state trace and hash, game-state probes,
// input record / replay, the scripted-run runner. Docs: docs/S5.md. Everything is off unless a [test] key (or its NXRT_TEST_* variable)
// asks for it, so a normal run pays nothing.
//
// The central idea is ONE frame boundary, tt::frame_boundary(), called once per game frame by whichever source is configured
// ([test] frame_source): the game hook (an entry hook on the game's frame update, needs an entry in the sites
// file compiled into the lifted game), the runtime's diag_frame_tick (the present path), an SVC counter
// (bring-up: svc:N = a pseudo frame every N SVCs of the first guest thread) or the caller itself (manual). At each boundary:
//   1. the virtual clock steps (deterministic mode),
//   2. the game state is read from guest memory (probes) and one trace line + state hash is written,
//   3. the script runner / replay decides the pad for the next frame, a recorder notes the live pad,
//   4. the pad is published to the input source (hid) and the raw mouse accumulator.
// So the state in trace line N is the state at the START of frame N and the pad of frame N acts on frame N.
//
// Configuration keys ([test] section, env NXRT_TEST_<KEY>):
//   script=<file.nxs>      run a scripted run (tt_script.h); exit code 0 pass / 1 fail / 5 freeze / 124 wall timeout
//   regions=<file.tsv>     name table for @area/@warp (default: regions.tsv next to the script, else none)
//   out=<dir>              output dir (default "testout"): result.json, trace.jsonl, input.nxin, shots/
//   deterministic=1        fixed RNG seed + virtual clock: one frame = clock_step_ticks (default 1/60 s at the system tick rate, nxrt/clock.h)
//   seed=<n>               RNG seed (default 0x5EED5EED)
//   frame_source=hook|tick|svc:N|manual   (default hook)
//   trace=<file>           per-frame JSON lines (default <out>/trace.jsonl when anything is enabled; "none" disables)
//   record=<file>          record the live pad of every frame (input trace, tt_inputfile.h)
//   replay=<file>          feed that input trace instead of the live pad; with replay_exit=1 the run ends where the trace ends
//   hash_regions=<list>    memory folded into the per-frame hash: comma separated 0xADDR+0xSIZE; "module:<name>" = the RW pages of a module
//   max_frames=<n>         end the run (fail) after n frames; wall_timeout_s=<s> (default 900 with a script) ends it with exit code 124
//   freeze_exit=1          a watchdog freeze ends the run with exit code 5 (default on with a script)
//   shot_dir=<dir>         default <out>/shots; require_shots=1 turns a missing screenshot provider into a failure
//   game_base=<addr>       base of the game's main module (default 0x7100000000)
// Probe layout overrides ([probe] section): state_manager=<addr> (a CStateManager the hooks did not see), see the game's probes
// (GameProbes below).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>
#include <string>

#include "nxrt/abi.h"
#include "nxrt/tt_inputfile.h"
#include "nxrt/tt_script.h"

namespace nxrt::tt {

// ---- the game state the probes read (docs/S5.md has the offsets and how they were established) ------------------------------
struct GameState {
    bool have_world = false;       // world/area ids read from gpGameState
    bool have_player = false;      // position read through the CStateManager the hooks captured
    bool have_igt = false;
    bool have_hp = false;
    uint8_t world[16] = {};        // memory order
    uint8_t area[16] = {};
    float pos[3] = {};
    float vel[3] = {};             // finite difference of pos between two frame boundaries, units per second at 60 frames/s (0 at the first sample)
    float yaw = 0;                 // radians, from the transform's forward column
    float hp = 0;
    double igt = 0;                // CGameState play time, seconds
    uint32_t move_state = 0;       // EPlayerMovementState
    // read from the game's own state (S13), valid with have_phys / have_aim / have_ps:
    bool have_phys = false;        // vel[] is the physics object's world velocity (CPhysicsActorMP1::GetVelocityWR), not a position difference
    float speed = 0;               // |vel|
    bool have_aim = false;
    float pitch = 0;               // free-look pitch angle, radians, + = up (CPlayerMP1 + 0x5cc)
    int32_t cam_state = -1;        // EPlayerCameraState (0 first person)
    int32_t morph_state = -1;      // morph transition state word (0 = unmorphed)
    int32_t orbit_state = -1;      // EPlayerOrbitState (0 = not locked on)
    bool have_cam = false;         // the first-person camera object's heading and pitch (what the player sees; differs from yaw/pitch under free look)
    float cam_yaw = 0, cam_pitch = 0;
    bool have_ps = false;
    int32_t beam = -1;             // current beam, game numbering 0 power 1 ice 2 wave 3 plasma
    int32_t visor = -1;            // 0 combat 1 x-ray 2 scan 3 thermal
    int32_t missiles = -1;         // item 4 amount
    bool have_tod = false;         // games with an in-game clock (Pokopia): the time of day, hours in [0, 24)
    float tod = 0;
    bool have_prog = false;        // a game's own progress counter (Pokopia: tutorials seen), monotonic over a playthrough
    int32_t prog = 0;
};

struct Checkpoint { std::string label; uint64_t frame = 0; uint64_t hash = 0; };

// ---- lifecycle ---------------------------------------------------------------------------------------------------------------
// Called from nxrt::init. Reads [test]; returns whether any test feature is on. Idempotent.
bool init();
// Called from nxrt::shutdown (and by end_run): flushes the trace, the recording and result.json (if not already written).
void shutdown();
bool enabled();
bool deterministic();
uint64_t frame();                          // frame boundaries seen so far
const std::string& out_dir();

// ---- the frame boundary ---------------------------------------------------------------------------------------------------------
// `c` is the live context when called from a hook (needed only for @warp, which calls guest functions; the context is saved and restored).
void frame_boundary(::Ctx* c = nullptr);
// Registers the game's entry hooks of the frame source and of the state-pointer capture (GameProbes::install_hooks).
void install_game_hooks();
// The state-pointer capture, called from the ProcessInput hook (x0 = CPlayerMP1*, x2 = CStateManager&); tests call it directly.
void set_player_pointers(uint64_t player, uint64_t state_manager);
// The context of the hook that is running the frame boundary: lets read_state call guest functions (the physics velocity). Null outside a hook.
void set_call_context(::Ctx* c);

// ---- state ---------------------------------------------------------------------------------------------------------------------------
bool read_state(GameState* out);           // the probes, now (guest memory); false when nothing could be read
uint64_t last_hash();                      // the per-frame hash of the last boundary
// Probe layout (main-relative offsets, see docs/S5.md); tests point these at host memory built with the same shape.
struct ProbeLayout {
    uint64_t game_state_global = 0;        // address of the gpGameState variable (a pointer to CGameStateMP1); 0 = game_base + 0x459cc70
    uint64_t state_manager = 0;            // CStateManager*, 0 = whatever the ProcessInput hook captured
    uint64_t player = 0;                   // CPlayerMP1*, 0 = derived from the state manager (GameLogic + 0x18) or the hook
    uint64_t game_globals = 0;             // address of the CGameGlobalObjects static (its +0 = CResourceFactory*); 0 = game_base + 0x4578710
};
void set_probe_layout(const ProbeLayout& l);

// ---- the virtual clock and the seeded random stream (deterministic mode) -----------------------------------------------------
// init installs them as the guest's clock (nxrt/clock.h) and random source (nxrt/entropy.h); shutdown takes them out again.
// The script step `@rate <hz>`: the game's step becomes exactly 1/hz for every frame from here on (nothing waits and no host time enters it), 0 = the
// configured mode again. Areas are loaded at the real rate and measured at a fixed one, in one process.
void set_clock_rate(double hz);

// ---- plumbing --------------------------------------------------------------------------------------------------------------------------
// Screenshots (@shot) go through nxrt/screenshot.h. Called at the end of a run, before the process exits: waits until the screenshots the
// provider still writes in the background are on disk (the graphics layer registers it; none: nothing to wait for).
void set_screenshot_flush(void (*fn)());
// Where a replayed / scripted mouse impulse goes (the PC-input layer registers input::mouse_delta_add here; counts per frame).
using MouseSinkFn = void (*)(double dx, double dy);
void set_mouse_sink(MouseSinkFn fn);
// Where a scripted key step goes (the play layer registers its key/button/wheel handler here). down=true presses the named input,
// false releases it. Return false when the name is unknown or cannot be delivered: the run then fails at that script line.
// Keys still down when the run ends (pass, fail or shutdown) are released with down=false.
using KeySinkFn = bool (*)(const char* input_name, bool down);
void set_key_sink(KeySinkFn fn);
bool finished();                           // the run has ended (result written)
int exit_code();
// Ends the run now. verdict: "pass" "fail" "crash" "freeze" "timeout" "error". Writes result.json, flushes the files and ends the process
// with `code`.
void end_run(int code, const char* verdict, const std::string& message);
const std::vector<Checkpoint>& checkpoints();

// ---- the game's probes (nxrt/game.h Info::probes) -----------------------------------------------------------------------------------
// What the test tooling knows of one game: its frame boundary and player-input hooks, how to read the state, and the script steps that need
// the game's own functions (@warp @give @grant @call @objs @probe @rays @watch). The entry points above and in tt_internal.h forward here.
// The game's resource loader as the probe reads it (S42: test warps wait for an idle loader). Unknown: not readable (no game globals yet).
enum class LoaderState { Unknown, Busy, Idle };

struct GameProbes {
    void (*install_hooks)() = nullptr;
    bool (*read_state)(GameState* out) = nullptr;
    void (*set_probe_layout)(const ProbeLayout& l) = nullptr;
    void (*set_player_pointers)(uint64_t player, uint64_t state_manager) = nullptr;
    void (*set_call_context)(::Ctx* c) = nullptr;
    bool (*have_state_manager)() = nullptr;
    LoaderState (*loader_state)(uint32_t* pending) = nullptr;   // *pending (may be null) = the queued loads when readable
    bool (*warp)(::Ctx* c, const uint8_t world[16], const uint8_t area[16]) = nullptr;
    bool (*grant)(int64_t item, int32_t amount, int32_t capacity) = nullptr;
    bool (*give_item)(int item, int amount) = nullptr;
    bool (*call_guest)(::Ctx* c, uint64_t offset, const std::string& args, uint64_t* result, std::string* err) = nullptr;
    bool (*dump_objects)(const std::string& classes, const std::string& out_path, std::string* err) = nullptr;
    bool (*probe_positions)(::Ctx* c, const std::string& in_path, const std::string& out_path, std::string* err) = nullptr;
    bool (*cast_rays)(::Ctx* c, const std::string& in_path, const std::string& out_path, std::string* err) = nullptr;
    bool (*watch_start)(::Ctx* c, int what) = nullptr;
    void (*watch_stop)() = nullptr;
    void (*watch_flush)(uint64_t frame) = nullptr;
};

}  // namespace nxrt::tt

// The adapter to the hid service's input source (nxrt/input_source.h) lives in tt_hid.cpp.
