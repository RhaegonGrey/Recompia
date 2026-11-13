#requires -Version 7.0
# Builds the native Pokemon-cap runtime, launcher and compatible shader translator.
# Example: pwsh -File .\Build-Recompia-PokemonCap.ps1
[CmdletBinding()]
param(
    [string]$CheckoutDir = (Join-Path (Get-Location).Path 'Recompia-PokemonCap'),
    [string]$OutputDir = '',
    [switch]$Clean,
    [switch]$SkipRuntime,
    [switch]$SkipLauncher,
    [switch]$SkipHarness
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$branch = 'feature/pokemon-cap-launcher-20261113'
$repo = 'https://github.com/RhaegonGrey/Recompia.git'

function Need([string]$tool) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "Missing prerequisite: $tool" }
}
function Run([string]$tool, [string[]]$argv) {
    Write-Host ("> " + $tool + " " + ($argv -join ' ')) -ForegroundColor Cyan
    & $tool @argv
    if ($LASTEXITCODE -ne 0) { throw "$tool failed: exit $LASTEXITCODE" }
}
function Stage([string]$from, [string]$to) {
    if (-not (Test-Path -LiteralPath $from -PathType Leaf)) { throw "Build output missing: $from" }
    Copy-Item -LiteralPath $from -Destination $to -Force
}
if (-not $IsWindows) { throw 'Windows x64 is required.' }
Need git
$CheckoutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($CheckoutDir)
if (-not $OutputDir) { $OutputDir = Join-Path (Split-Path -Parent $CheckoutDir) 'Recompia-PokemonCap-built' }
$OutputDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutputDir)
if (-not (Test-Path -LiteralPath $CheckoutDir)) {
    Run git @('clone','--branch',$branch,'--single-branch',$repo,$CheckoutDir)
} else {
    if (-not (Test-Path -LiteralPath (Join-Path $CheckoutDir '.git'))) {
        throw "Not a Git checkout: $CheckoutDir"
    }
    $remote = (& git -C $CheckoutDir remote get-url origin).Trim()
    if ($LASTEXITCODE -ne 0 -or $remote -notmatch '(?i)github\.com[:/]RhaegonGrey/Recompia(\.git)?$') {
        throw "Unexpected repository origin: $remote"
    }
    if (@(& git -C $CheckoutDir status --porcelain).Count -gt 0) {
        throw 'Checkout has local changes; refusing to overwrite them.'
    }
    Run git @('-C',$CheckoutDir,'fetch','origin',$branch)
    $current = (& git -C $CheckoutDir branch --show-current).Trim()
    if ($current -ne $branch) { Run git @('-C',$CheckoutDir,'switch',$branch) }
    Run git @('-C',$CheckoutDir,'merge','--ff-only','FETCH_HEAD')
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

# Windows native runtime: MSVC x64 + clang-cl, Ninja, CMake and texture libraries.
if (-not $SkipRuntime) {
    . (Join-Path $CheckoutDir 'tools\scripts\msvc.ps1')
    $vcvars = Get-VcVars64
    if (-not (Test-Path -LiteralPath $vcvars)) { throw 'Install Visual Studio C++ Build Tools and Windows SDK.' }
    Import-MsvcEnv $vcvars
    foreach ($tool in @('cmake','ninja','clang-cl')) { Need $tool }
    $textureDir = Join-Path $CheckoutDir 'tools\textures\build\release'
    if ($Clean -or -not (Test-Path (Join-Path $textureDir 'astc2bc7.lib')) -or
        -not (Test-Path (Join-Path $textureDir 'bc7e_ispc.lib'))) {
        & (Join-Path $CheckoutDir 'tools\textures\scripts\fetch_third_party.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'Texture dependency fetch failed.' }
        & (Join-Path $CheckoutDir 'tools\textures\scripts\build.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'Texture libraries failed to build.' }
    }
    $buildDir = Join-Path $CheckoutDir 'build\pokemon-cap-rtdll'
    if ($Clean -and (Test-Path $buildDir)) { Remove-Item -LiteralPath $buildDir -Recurse -Force }
    $cfg = @(
        '-S',(Join-Path $CheckoutDir 'game-build'),'-B',$buildDir,'-G','Ninja',
        '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_COMPILER=clang-cl',
        '-DCMAKE_CXX_COMPILER=clang-cl','-DPKP_RT_DLL=ON',
        '-DPKP_RELEASE=ON','-DPKP_VERSION=dev'
    )
    if (Get-Command sccache -ErrorAction SilentlyContinue) {
        $cfg += @('-DCMAKE_C_COMPILER_LAUNCHER=sccache','-DCMAKE_CXX_COMPILER_LAUNCHER=sccache')
    }
    Run cmake $cfg
    Run cmake @('--build',$buildDir,'--target','pkp_rt')
    Stage (Join-Path $buildDir 'pkp_rt.dll') (Join-Path $OutputDir 'pkp_rt.dll')
    Stage (Join-Path $buildDir 'pkp_rt.lib') (Join-Path $OutputDir 'pkp_rt.lib')
    if (Test-Path (Join-Path $buildDir 'libusb-1.0.dll')) {
        Stage (Join-Path $buildDir 'libusb-1.0.dll') (Join-Path $OutputDir 'libusb-1.0.dll')
    }
}

# Tauri launcher: preserves Settings > Gameplay > Pokemon cap choices 25..63.
if (-not $SkipLauncher) {
    foreach ($tool in @('node','npm','cargo','rustc')) { Need $tool }
    $launcher = Join-Path $CheckoutDir 'launcher'
    Push-Location $launcher
    try {
        Run npm @('ci')
        Run node @('scripts/test-settings-schema.mjs')
        Run npm @('run','tauri','build')
    } finally { Pop-Location }
    Stage (Join-Path $launcher 'src-tauri\target\release\pokopia-launcher.exe') (Join-Path $OutputDir 'PokopiaLauncher.exe')
}

# The Set up wizard requires the --serve-capable self-contained translator.
if (-not $SkipHarness) {
    $repair = Join-Path $PSScriptRoot 'Fix-Recompia-Shader-Translator.ps1'
    if (-not (Test-Path -LiteralPath $repair)) {
        throw "Please save Fix-Recompia-Shader-Translator.ps1 beside this script: $repair"
    }
    New-Item -ItemType Directory -Path (Join-Path $OutputDir 'tools') -Force | Out-Null
    & $repair -InstallDir $OutputDir -CheckoutDir $CheckoutDir
    if ($LASTEXITCODE -ne 0) { throw 'Building the shader translator failed.' }
}

@"
Pokemon cap build from $repo / $branch
- pkp_rt.dll is the native runtime; pkp_rt.lib is used to link a compatible pkp.exe.
- PokopiaLauncher.exe contains the Pokemon cap selector (25..63).
- tools/spirv_harness contains the --serve shader translator.
This is NOT a complete standalone Recompia release.
Back up your existing installation before replacing components.
Do not mix binaries from incompatible releases. No game files are included.
"@ | Set-Content -LiteralPath (Join-Path $OutputDir 'BUILD-README.txt')
Write-Host "Build artifacts: $OutputDir" -ForegroundColor Green
