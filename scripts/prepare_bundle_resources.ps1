<#
.SYNOPSIS
  Single entry point that assembles everything app/desktop/src-tauri/resources/ needs
  before `npm run tauri build` (Phase 5.2): the freshly-built mediatool-core.exe, the
  vendored FFmpeg build, and the bundled Python runtime.

.DESCRIPTION
  Run this after building the C++ core in Release (`cmake --build --preset
  windows-mingw-release`) and before packaging. Individual steps also work standalone --
  see scripts/vendor_ffmpeg.ps1 and scripts/vendor_python_runtime.ps1.

.NOTES
  Windows/PowerShell only. Not yet exercised in CI -- see Phase 5.4.
#>

[CmdletBinding()]
param(
    [switch]$SkipFfmpeg,
    [switch]$SkipPython
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$CoreExe = Join-Path $RepoRoot "build\windows-mingw-release\app\core\mediatool-core.exe"
$ResourcesRoot = Join-Path $RepoRoot "app\desktop\src-tauri\resources"

if (-not (Test-Path $CoreExe)) {
    throw "$CoreExe does not exist -- build the Release preset first: cmake --build --preset windows-mingw-release"
}

New-Item -ItemType Directory -Force -Path $ResourcesRoot | Out-Null
Copy-Item -Path $CoreExe -Destination (Join-Path $ResourcesRoot "mediatool-core.exe") -Force
Write-Host "Copied mediatool-core.exe into $ResourcesRoot"

if (-not $SkipFfmpeg) {
    & (Join-Path $PSScriptRoot "vendor_ffmpeg.ps1")
} else {
    Write-Host "Skipping FFmpeg vendoring (-SkipFfmpeg)."
}

if (-not $SkipPython) {
    & (Join-Path $PSScriptRoot "vendor_python_runtime.ps1")
} else {
    Write-Host "Skipping Python runtime vendoring (-SkipPython)."
}

Write-Host "app/desktop/src-tauri/resources/ is ready. From app/desktop, run:"
Write-Host "  npm run tauri build -- --config ..\..\app\desktop\src-tauri\tauri.release.conf.json"
Write-Host "(see docs/development.md 'Packaging' for why the resources config lives in a separate --config file, not the base tauri.conf.json)"
