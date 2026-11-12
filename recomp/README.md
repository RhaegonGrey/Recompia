# nxrecomp: AArch64 to C++ static recompiler

Turns the code of a Nintendo Switch program (NSO modules, or any static AArch64 ELF) into C++ that builds natively on Windows
x64 with clang-cl. The translation is done by deterministic code. There is no inline assembly and no
interpreter. It is the CPU half of the Pokopia PC port; nothing here is specific to that game except `games/pkp/`.

Licence: GPL-3.0-or-later (`LICENSE`). Borrowed code and its licences: `THIRD_PARTY.md`.

## What it produces

For each guest function `f_<address>(Ctx *c)`, working on the register context of `include/nxrecomp/ctx.h`. Guest memory is
host memory (the image is mapped at its guest address), direct branches are `goto`, calls are direct C calls, indirect calls go
to the runtime's `call_indirect`, jump tables become bounded `switch` statements, C++ landing pads become resume entries
`u_<address>(c, pad)`, and each module gets a descriptor `nxmod_<name>` (function table, dynamic symbols, resume table, unwind
tables). Output is sharded into a fixed number of translation units for the build.

**The generated-code ABI is `include/nxrecomp/ctx.h`** (include with `#include <nxrecomp/ctx.h>` and
`-I recomp/include`). The runtime implements what the header declares as runtime-provided: `call_indirect`, `host_import`,
`guest_trap`, `guest_unimpl`, `guest_svc`, `guest_mrs/msr`, `stx_diag`, `nx_catch_call`, and the three site hooks. The ABI
version is `NXRECOMP_ABI_VERSION` (3).

## Use

Python is `tools\.venv\Scripts\python.exe` at the repository root (3.13; packages in `requirements.txt`). Build tools: LLVM
23 clang-cl, CMake, Ninja, sccache; `scripts\nx_env.cmd <command>` runs a command with the MSVC environment set up.

    python -m nxrecomp lift  <module.nso|prog.elf> <outdir> [--game pkp] [--base 0x7100000000] [--name main] [--index N]
                             [--shards 16] [--jobs 8] [--patches FILE] [--sites FILE] [--spin-yield]
                             [--locals off|exact|aapcs]
    python -m nxrecomp cover <module.nso|prog.elf> [--game pkp]      per-instruction-word translation coverage of .text
    python -m nxrecomp.funcs <module> --out functions.tsv             function boundaries and the evidence for each
    python -m nxrecomp build-info [--game pkp]                       the game's module table and the compile flags (JSON)

In a CMake build (the game), `cmake/NxRecomp.cmake` gives `nxrecomp_add_module(<target> INPUT <file> ...)`, which
lifts as a build step and builds the result as a static library (flags from `nxrecomp/compile_flags.py`, shared with the
player build: C++20, `-fno-strict-aliasing`, `-ffp-contract=off`, `-mcx16`; optnone for functions over `optnone_insns`
instructions; `NX_USE_LAUNCHER` adds a time-limit fallback; `NX_SPIN_YIELD` passes `--spin-yield`; `NX_LOCALS`, or the
environment variable of that name at configure time, passes `--locals`), and
`nxrecomp_build_info(<game> <prefix>)` reads the module table of `games/<game>/config.py`. Presets:
`cmake --preset default`.

## How it is built

- `nxrecomp/image.py` loads a module (NSO, smo-style ELF, or static ELF) into one `Image`; `funcs.py` finds function
  boundaries (`.eh_frame` and sized symbols first); `eh.py` reads CFI and C++ LSDAs; `jumptables.py` resolves `br` tables;
  `env.py` holds the per-module state, `emit_function.py` turns one function into C++, `driver.py` runs discovery, the worker pool and the
  sharding, `module_emit.py` writes `funcs.h` and the module descriptor, `sites.py` reads the site lists.
- Instruction lifting has two tiers. Tier 1 (`lift/a64.py`) decodes bit fields and is the reference where it applies. Tier 2
  (`lift/tier2/`) is Capstone-driven and covers what tier 1 does not: LSE atomics, crypto, the long tail of AdvSIMD, structure
  loads and stores. The files `x_*.py` there override or add handlers; the ported emitters beside them are kept as the base.
- `locals.py` (`--locals`, game config `reg_locals`, default `off`) keeps the guest X registers and NZCV in C locals of each
  lifted function and syncs them with the context only where other code can see it: before every call, trap and runtime
  callout, at every return and tail exit. `exact` leaves the context there as the eager code does; `aapcs` also keeps NZCV out
  of the context at calls and returns, as AAPCS64 allows, and the lift log lists the flag reads where that could differ; `off`
  is the eager code. Between two sync points the context of a running thread is behind the locals, so a report that reads
  another thread's context shows its registers as of the last call. The player build takes the mode from the game config
  (`PKP_LOCALS` overrides it for a test build) and keeps it in its build stamp.
- `spin.py` finds busy-wait loops (a short backward branch whose loop only loads, computes and branches and carries nothing
  from one pass to the next) and lifts their loads as relaxed atomic loads (`rdp*`, `rdvp`, `nx::ld_poll`), so the compiler
  can neither hoist them nor drop the loop. `--spin-yield` (game config `spin_yield`) also calls `nx_spin_hint(2)` at each
  loop head. The lift log prints the counts per module.
- `games/<name>/config.py` is the per-game seam (module list and bases, function overrides, extra entries, patches).

Lifting and compiling the game itself: `scriptsuild_game.cmd pkp` (configure, lift and compile `games/pkp`; the game's NSOs
come from `-DPKP_EXEFS`). Gaps that remain are listed with numbers in `docs/GAPS.md`; numbered bug notes are in `docs/debug/`.
