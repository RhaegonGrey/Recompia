#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the macOS release: one zip whose root is PokopiaRecomp-<version>/ holding the launcher binary, the payload
# make-payload.sh staged and the runtime dylib. No .app and no signature: the player unpacks the zip anywhere and starts the
# launcher from there, as on Windows and Linux, and the note in the zip walks them through the one-time Open Anyway macOS asks
# for. The package ships no game code: Set up lifts and compiles pkp from the player's own dump.
#
#   launcher/scripts/make-mac.sh <play folder with PokopiaLauncher> <payload dir> [version]
#
# Needs a production launcher build (npm run tauri build -- --no-bundle) and the runtime dylib (PKP_RT_DLL=ON, docs/macos-port.md).
set -eu
here=$(cd "${1:?usage: make-mac.sh <play folder> <payload dir> [version]}" && pwd)
payload=$(cd "${2:?usage: make-mac.sh <play folder> <payload dir> [version]}" && pwd)
repo=$(cd "$(dirname "$0")/../.." && pwd)
# the release version: the launcher's manifest, unless the caller names one (the release workflow passes the tag's)
version=${3:-$(sed -nE 's/^[[:space:]]*"version": "([^"]+)".*/\1/p' "$repo/launcher/src-tauri/tauri.conf.json" | head -1)}
zip="$here/Recompia-$version-macos-arm64.zip"

[ -x "$here/PokopiaLauncher" ] || { echo "no launcher binary at $here/PokopiaLauncher (npm run tauri build -- --no-bundle)" >&2; exit 1; }
[ -d "$payload/tools/build" ] || { echo "no payload at $payload (launcher/scripts/make-payload.sh)" >&2; exit 1; }
[ -f "$repo/work/build/rtb/libpkp_rt.dylib" ] || { echo "no runtime dylib at work/build/rtb (cmake --build --target pkp_rt)" >&2; exit 1; }

# The layout the launcher expects (paths::exe_dir): tools/ next to the binary, and the runtime dylib twice — beside the binary,
# as pkp_rt.dll sits beside PokopiaLauncher.exe on Windows, and in tools/build/sdk, which is where Set up copies it from into the
# game folder.
stage=$(mktemp -d)
root="$stage/PokopiaRecomp-$version"
mkdir -p "$root/tools/build/sdk"
cp "$here/PokopiaLauncher" "$root/"
cp -R "$payload/." "$root/"
# The launcher runs the apphost itself (host.rs: apphost.is_file() -> command(&apphost)), and a payload that came back through
# an artifact has no mode: without this the shipped zip carries tools/harness/spirv_harness as 0644 and Set up fails EACCES.
chmod +x "$root/tools/harness/spirv_harness"
cp "$repo/work/build/rtb/libpkp_rt.dylib" "$root/libpkp_rt.dylib"
cp "$repo/work/build/rtb/libpkp_rt.dylib" "$root/tools/build/sdk/libpkp_rt.dylib"
# The fetched per-machine tools (python-build-standalone, lz4, capstone) belong to a game folder: Set up fetches them there. A
# payload that came back from a build machine that ran the launcher carries one under tools/macos; in the release it is 78 MB
# that nothing reads (tools_dir_for() is the game folder). Bytecode caches are build leftovers as well.
rm -rf "$root/tools/macos"
find "$root" -name __pycache__ -type d -prune -exec rm -rf {} + 2>/dev/null || true
# An artifact round trip leaves com.apple.provenance on the files; harmless, but no reason to ship it.
xattr -cr "$root" 2>/dev/null || true

cat > "$root/How to open Recompia.txt" <<'TXT'
Recompia is not notarized by Apple, so macOS refuses to open it the first time. This is expected.

1. Unpack the zip anywhere you like (the folder can stay where it is; nothing is installed).
2. Start PokopiaLauncher once and let macOS say it cannot be opened. Close that dialog.
3. Open System Settings, Privacy and Security, and scroll down to the Security section.
   There is a line about Pokopia Launcher being blocked: press "Open Anyway", then "Open".
4. From then on it opens like any other program.

On macOS 14 and older you can also right-click PokopiaLauncher, choose Open, and confirm.

If System Settings shows no such line, remove the download flag in Terminal instead:

   xattr -dr com.apple.quarantine /path/to/PokopiaRecomp-<version>

The folder is the whole release: the launcher, tools/ (the lifter, the SDK and the shader translator), and libpkp_rt.dylib. A
game folder is made by the launcher's Set up from your own dump; no game code is shipped here.
TXT

rm -f "$zip"
ditto -c -k --sequesterRsrc --keepParent "$root" "$zip"
rm -rf "$stage"
echo "built $zip"
