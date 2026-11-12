Modtopia 0.6.1 (MIT, `LICENSE`): the mod loader core (`core/src`), the mod and host SDK headers (`sdk/include/modtopia`) and the
community function-name mappings for each game version (`mappings`), copied unchanged from the Modtopia 0.6.1 package; update by
replacing the files. The runtime's mods host (`runtime/src/mods/`, `nxrt/mods.h`) links the core as the static library `nxrt_modtopia`
and embeds the mappings; `CMakeLists.txt` and this file are ours.
