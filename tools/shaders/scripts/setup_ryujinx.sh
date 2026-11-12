#!/usr/bin/env bash
# Stage the three Ryujinx projects the translator needs into tools/shaders/third_party/ryubing (git-ignored).
# Source: the read-only checkout refs/ryubing (Ryubing fork, Codeberg archive a82350bb77, MIT). The build writes obj/bin
# next to the sources, so we build from this copy and never touch refs/. The Linux twin of setup_ryujinx.ps1.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
src=${1:-"$here/../../../refs/ryubing"}
dst="$here/../third_party/ryubing"

[ -d "$src/src" ] || { echo "no Ryujinx checkout at $src" >&2; exit 1; }

mkdir -p "$dst/src"
for p in Ryujinx.Common Spv.Generator Ryujinx.Graphics.Shader; do
    rm -rf "$dst/src/$p"
    mkdir -p "$dst/src/$p"
    # the sources without their build output (robocopy /MIR /XD obj bin in the PowerShell twin)
    ( cd "$src/src/$p" && tar -cf - --exclude=./obj --exclude=./bin . ) | ( cd "$dst/src/$p" && tar -xf - )
done
for f in Directory.Build.props Directory.Packages.props global.json nuget.config LICENSE.txt; do
    cp -f "$src/$f" "$dst/$f"
done
echo "staged Ryujinx sources in $dst"
