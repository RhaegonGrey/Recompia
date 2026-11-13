#requires -Version 7.0
# Fixes the shader translator --serve mismatch during launcher Set up.
# Example:
#   pwsh -File .\Fix-Recompia-Shader-Translator.ps1 -InstallDir 'X:\Switch Games\PokopiaRecomp-0.12.0'
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InstallDir,
    [string]$CheckoutDir = (Join-Path (Get-Location).Path 'Recompia-PokemonCap')
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$branch = 'feature/pokemon-cap-launcher-20261113'
$repo = 'https://github.com/RhaegonGrey/Recompia.git'
$ryubingCommit = 'a82350bb774f70fcbd41c9987bf67a3775409963'

function Run([string]$exe, [string[]]$argv) {
    Write-Host ("> " + $exe + " " + ($argv -join ' ')) -ForegroundColor Cyan
    & $exe @argv
    if ($LASTEXITCODE -ne 0) { throw "$exe failed: exit $LASTEXITCODE" }
}
if (-not $IsWindows) { throw 'Windows x64 is required.' }
foreach ($tool in @('git','dotnet')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Missing prerequisite: $tool (Git and .NET 10 SDK required)."
    }
}
if (-not ((& dotnet --list-sdks) -match '^10\.')) { throw '.NET 10 SDK is required.' }
$InstallDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($InstallDir)
$CheckoutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($CheckoutDir)
$toolsDir = Join-Path $InstallDir 'tools'
if (-not (Test-Path -LiteralPath $toolsDir -PathType Container)) {
    throw "Expected an installed launcher with tools folder: $toolsDir"
}
if (-not (Test-Path -LiteralPath $CheckoutDir)) {
    Run git @('clone','--branch',$branch,'--single-branch',$repo,$CheckoutDir)
}
if (-not (Test-Path -LiteralPath (Join-Path $CheckoutDir '.git'))) {
    throw "Not a Git checkout: $CheckoutDir"
}
$remote = (& git -C $CheckoutDir remote get-url origin).Trim()
if ($LASTEXITCODE -ne 0 -or $remote -notmatch '(?i)github\.com[:/]RhaegonGrey/Recompia(\.git)?$') {
    throw "Checkout must belong to your fork. Found: $remote"
}
$currentBranch = (& git -C $CheckoutDir branch --show-current).Trim()
if ($currentBranch -ne $branch) { throw "Expected branch $branch, found $currentBranch" }
Run git @('-C',$CheckoutDir,'fetch','origin',$branch)
if (@(& git -C $CheckoutDir status --porcelain).Count -eq 0) {
    Run git @('-C',$CheckoutDir,'merge','--ff-only','FETCH_HEAD')
} else {
    Write-Warning 'Checkout has local changes; leaving them untouched.'
}

# The same pinned Ryubing revision used in the Windows release workflow.
$ryubing = Join-Path $CheckoutDir 'refs\ryubing'
if (-not (Test-Path -LiteralPath (Join-Path $ryubing '.git'))) {
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ryubing) | Out-Null
    Run git @('clone','--filter=blob:none','https://git.ryujinx.app/ryubing/ryujinx.git',$ryubing)
}
Run git @('-C',$ryubing,'checkout','--detach',$ryubingCommit)
& (Join-Path $CheckoutDir 'tools\shaders\scripts\setup_ryujinx.ps1') -Ryubing $ryubing
if ($LASTEXITCODE -ne 0) { throw 'Staging the Ryubing shader sources failed.' }

$publish = Join-Path $CheckoutDir 'build\pokemon-cap-shader-translator'
$project = Join-Path $CheckoutDir 'tools\shaders\harness\SpirvHarness.csproj'
Run dotnet @('publish','-c','Release','-r','win-x64','--self-contained','true',$project,'-o',$publish)
$exe = Join-Path $publish 'spirv_harness.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Missing expected translator: $exe" }
$helpText = (& $exe --help 2>&1 | Out-String)
if ($LASTEXITCODE -ne 0 -or $helpText -notmatch '--serve') {
    throw 'New translator failed --serve compatibility check; installation unchanged.'
}
if (Get-Process -Name 'PokopiaLauncher','pokopia-launcher','spirv_harness' -ErrorAction SilentlyContinue) {
    throw 'Close the launcher and shader translator before replacing their files.'
}

# Keep the whole self-contained .NET publish together and back up the old copy.
$target = Join-Path $toolsDir 'spirv_harness'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$staging = Join-Path $toolsDir ('.spirv_harness-new-' + $stamp)
$backup = Join-Path $toolsDir ('spirv_harness.backup-' + $stamp)
New-Item -ItemType Directory -Path $staging | Out-Null
try {
    Get-ChildItem -LiteralPath $publish -Force | Copy-Item -Destination $staging -Recurse -Force
    $check = (& (Join-Path $staging 'spirv_harness.exe') --help 2>&1 | Out-String)
    if ($LASTEXITCODE -ne 0 -or $check -notmatch '--serve') {
        throw 'Copied shader translator failed verification; installation unchanged.'
    }
    $oldMoved = $false
    try {
        if (Test-Path -LiteralPath $target) {
            Move-Item -LiteralPath $target -Destination $backup
            $oldMoved = $true
        }
        Move-Item -LiteralPath $staging -Destination $target
    } catch {
        if ($oldMoved -and -not (Test-Path -LiteralPath $target) -and (Test-Path -LiteralPath $backup)) {
            Move-Item -LiteralPath $backup -Destination $target
        }
        throw
    }
    Write-Host "Updated: $target" -ForegroundColor Green
    if ($oldMoved) { Write-Host "Backup: $backup" }
    Write-Host 'Reopen the launcher and resume Set up. Saves and shader cache were not changed.'
} finally {
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
}
