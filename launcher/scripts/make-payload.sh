#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Stages everything the macOS bundle carries except the two files only macOS can build (the launcher binary and the runtime
# dylib): the lifter, the SDK sources, the builder, the self-contained shader harness, MoltenVK and the assets the game reads.
# It runs on any host, so the release workflow does this on a Linux runner and the macOS runner only compiles and signs.
#
#   launcher/scripts/make-payload.sh <out dir> [harness dir] [moltenvk dir]
#
# harness dir: a self-contained osx-arm64 publish of tools/shaders/harness
#              (dotnet publish tools/shaders/harness -c Release -r osx-arm64 --self-contained -o <dir>)
# moltenvk dir: a MoltenVK macOS distribution: libMoltenVK.dylib and LICENSE (MoltenVK-macos.tar.xz from its releases)
set -eu
out=${1:?usage: make-payload.sh <out dir> [harness dir] [moltenvk dir]}
repo=$(cd "$(dirname "$0")/../.." && pwd)
harness=${2:-$repo/work/harness}
moltenvk=${3:-$repo/work/moltenvk}

[ -d "$harness" ] || { echo "no harness publish at $harness (dotnet publish -r osx-arm64)" >&2; exit 1; }
[ -f "$moltenvk/libMoltenVK.dylib" ] || { echo "no libMoltenVK.dylib in $moltenvk" >&2; exit 1; }

# The game folder (pkp, pkp.ini, shader_cache, saves) is created by Set up in Application Support, never inside the bundle: this
# tree lands in Contents/MacOS/tools, which is where the launcher looks for it.
mkdir -p "$out/tools/build/sdk" "$out/tools/harness"
cp "$repo/game-build/release_b/pkp_build.py" "$repo/game-build/release_b/dump_hashes.json" "$out/tools/build/"
cp -R "$repo/recomp" "$out/tools/build/recomp"
find "$out/tools/build/recomp" -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null || true
# The shader pipeline: the corpus extractor and a self-contained publish of the harness (its own .NET runtime), so a player
# needs neither .NET nor a checkout.
cp "$repo/tools/shaders/ampere/extract_corpus.py" "$out/tools/"
cp -R "$harness/." "$out/tools/harness/"
chmod +x "$out/tools/harness/spirv_harness"   # the launcher runs the apphost itself, so the mode has to survive
cp "$repo/game-build/src/pkp_stub.cpp" "$repo/game-build/src/pkp_rt_api.h" "$out/tools/build/sdk/"
# macOS has no Vulkan loader of its own: MoltenVK ships with the package (with its licence) and Set up copies it next to pkp,
# which is where volk looks for libvulkan.1.dylib.
cp "$moltenvk/libMoltenVK.dylib" "$out/tools/build/sdk/libvulkan.1.dylib"
cp "$moltenvk/LICENSE" "$out/tools/build/sdk/LICENSE-MoltenVK.txt"
# What the game reads next to pkp: the prompt icons (generated here when the work folder does not have them) and the font.
if [ ! -d "$repo/work/assets/kenney_input_prompts" ]; then
    python3 "$repo/tools/kbm_icons.py" >/dev/null
fi
mkdir -p "$out/tools/build/sdk/kbm_icons" "$out/tools/build/sdk/fonts"
cp "$repo/work/assets/kenney_input_prompts/Keyboard & Mouse/Default/"*.png "$out/tools/build/sdk/kbm_icons/"
cp "$repo/runtime/assets/fonts/DejaVuSans.ttf" "$out/tools/build/sdk/fonts/"
cp "$repo/runtime/cmake/guest_segment.py" "$out/tools/build/sdk/"
# No Python in the bundle: Set up fetches python-build-standalone and the lz4 and capstone wheels into tools/macos, as the
# Linux package does (host.rs prepare_tools). macOS compiles with the Apple clang of the Command Line Tools.

echo "staged the payload in $out"
