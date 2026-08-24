# Stages this repo's own build outputs into app/desktop/src-tauri/resources/, ready for
# `tauri build` to bundle them (see tauri.conf.json's `bundle.resources` and
# docs/phase-7.md "Resource discovery"). Run from the repository root, after building the
# C++ core in Release configuration.
#
# This script copies ONLY what this repository builds itself. It does not fetch FFmpeg or
# a Python distribution -- those are third-party binaries with their own versioning and
# licensing, and app/desktop/src-tauri/resources/README.md documents exactly what a release
# engineer places under bin/ and python/ by hand. Running `tauri build` without doing that
# produces an installer whose FFmpeg discovery falls back to the user's PATH (see
# engines/ffmpeg/FFmpegDiscovery.h) rather than a bundled copy -- correct for testing this
# script and the rest of the packaging, not what should ship as a release.

param(
    [string]$Preset = "windows-mingw-release",
    [string]$RepoRoot = (Resolve-Path "$PSScriptRoot\..").Path
)

$ErrorActionPreference = "Stop"

$CoreBinary = Join-Path $RepoRoot "build\$Preset\app\core\mediatool-core.exe"
$DownloaderDir = Join-Path $RepoRoot "python\downloader"
$ResourcesDir = Join-Path $RepoRoot "app\desktop\src-tauri\resources"

if (-not (Test-Path $CoreBinary)) {
    throw "mediatool-core.exe not found at $CoreBinary -- build it first:`n" +
          "  cmake --preset $Preset`n  cmake --build --preset $Preset"
}

Write-Host "Staging release resources into $ResourcesDir"

New-Item -ItemType Directory -Force -Path $ResourcesDir | Out-Null
Copy-Item $CoreBinary -Destination $ResourcesDir -Force
Write-Host "  copied mediatool-core.exe"

$DownloaderDest = Join-Path $ResourcesDir "downloader"
New-Item -ItemType Directory -Force -Path $DownloaderDest | Out-Null
Get-ChildItem -Path $DownloaderDir -Filter "*.py" -File | ForEach-Object {
    Copy-Item $_.FullName -Destination $DownloaderDest -Force
}
Write-Host "  copied python/downloader/*.py"

$BinDir = Join-Path $ResourcesDir "bin"
$PythonDir = Join-Path $ResourcesDir "python"
$missing = @()
if (-not (Test-Path (Join-Path $BinDir "ffmpeg.exe"))) { $missing += "bin\ffmpeg.exe" }
if (-not (Test-Path (Join-Path $BinDir "ffprobe.exe"))) { $missing += "bin\ffprobe.exe" }
if (-not (Test-Path $PythonDir)) { $missing += "python\ (an embeddable Python with yt-dlp installed)" }

if ($missing.Count -gt 0) {
    Write-Warning "Not staged by this script (place by hand -- see resources/README.md):"
    $missing | ForEach-Object { Write-Warning "  $_" }
    Write-Warning "Without these, the packaged app falls back to the user's system PATH " +
                  "for FFmpeg and requires MEDIATOOL_PYTHON_PATH to be set for downloads -- " +
                  "fine for testing packaging, not what a release should ship."
}

Write-Host "Done. Run 'npm run build' in app/desktop to produce the installer."
