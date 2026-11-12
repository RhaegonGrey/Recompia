// SPDX-License-Identifier: GPL-3.0-or-later
// pkp.exe of release model (b): built on the player's PC (tools/build/pkp_build.py, bundled Zig) from this file and the four libraries
// lifted out of their own dump. All it does is hand the lifted module tables to the runtime in pkp_rt.dll (pkp_rt_api.h).
#include <stdint.h>
#include "pkp_rt_api.h"
#include "nx_modules.gen.h"     // NX_MODULES: the module list of the game config (nxrecomp modheader.py), written by pkp_build.py

extern "C" {
#define NX_MODULE_DECL(name, file, base) extern const struct NxModule nxmod_##name; extern const uint32_t nxmod_##name##_abi;
NX_MODULES(NX_MODULE_DECL)
#undef NX_MODULE_DECL
}

// The API this exe was linked for, exported so the launcher can read it from the file (pkp_rt_api.h). Windows needs the
// __declspec; a Mach-O executable exports its globals anyway.
#if defined(_WIN32)
#define PKP_EXPORT __declspec(dllexport)
#else
#define PKP_EXPORT __attribute__((visibility("default")))
#endif
extern "C" PKP_EXPORT const uint32_t pkp_rt_api = PKP_RT_API;

// The manifest (res/pkp.manifest) sets the UTF-8 code page, so argv is UTF-8 like the runtime expects.
int main(int argc, char** argv) {
    static const struct NxModule* const nx[] = {
#define NX_MODULE_NX(name, file, base) &nxmod_##name,
        NX_MODULES(NX_MODULE_NX)
#undef NX_MODULE_NX
    };
    static const uint32_t* const abi[] = {
#define NX_MODULE_ABI(name, file, base) &nxmod_##name##_abi,
        NX_MODULES(NX_MODULE_ABI)
#undef NX_MODULE_ABI
    };
    static const PkpRtModules mods = {sizeof(PkpRtModules), PKP_RT_API, sizeof nx / sizeof nx[0], nx, abi};
    return pkp_rt_main(argc, argv, &mods);
}
