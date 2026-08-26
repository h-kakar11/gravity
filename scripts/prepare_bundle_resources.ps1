<#
.SYNOPSIS
  Single entry point that assembles everything app/desktop/src-tauri/resources/ needs
  before `npm run tauri build` (Phase 5.2): the freshly-built mediatool-core.exe,
  WebView2Loader.dll, the vendored FFmpeg build, and the bundled Python runtime.

.DESCRIPTION
  Run this after building the C++ core in Release (`cmake --build --preset
  windows-mingw-release`) AND after building the Tauri/Rust side at least once (`cargo
  build --release` from app/desktop/src-tauri, or a prior `npm run tauri build`/`tauri
  dev`) -- WebView2Loader.dll only exists once that Rust build has produced it. Individual
  steps also work standalone -- see scripts/vendor_ffmpeg.ps1 and
  scripts/vendor_python_runtime.ps1.

.NOTES
  Windows/PowerShell only. Not yet exercised in CI -- see Phase 5.4.

  Why WebView2Loader.dll needs to be bundled explicitly at all: on the MSVC target Tauri's
  own bundler auto-includes it, but on the GNU/MinGW target this project uses (see
  docs/development.md), that auto-copy doesn't happen -- the DLL is compiled into
  target/release/ by cargo (via the webview2-com-sys crate) but never makes it into the
  installed app, so the packaged exe fails at launch with "WebView2Loader.dll was not
  found." Bundling it the same way as mediatool-core.exe closes that gap.
#>

[CmdletBinding()]
param(
    [switch]$SkipFfmpeg,
    [switch]$SkipPython
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$CoreExe = Join-Path $RepoRoot "build\windows-mingw-release\app\core\mediatool-core.exe"
$WebView2Loader = Join-Path $RepoRoot "app\desktop\src-tauri\target\release\WebView2Loader.dll"
$ResourcesRoot = Join-Path $RepoRoot "app\desktop\src-tauri\resources"

if (-not (Test-Path $CoreExe)) {
    throw "$CoreExe does not exist -- build the Release preset first: cmake --build --preset windows-mingw-release"
}

if (-not (Test-Path $WebView2Loader)) {
    throw "$WebView2Loader does not exist -- build the Tauri/Rust side at least once first: cargo build --release --manifest-path app\desktop\src-tauri\Cargo.toml"
}

New-Item -ItemType Directory -Force -Path $ResourcesRoot | Out-Null
Copy-Item -Path $CoreExe -Destination (Join-Path $ResourcesRoot "mediatool-core.exe") -Force
Write-Host "Copied mediatool-core.exe into $ResourcesRoot"
Copy-Item -Path $WebView2Loader -Destination (Join-Path $ResourcesRoot "WebView2Loader.dll") -Force
Write-Host "Copied WebView2Loader.dll into $ResourcesRoot"

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
