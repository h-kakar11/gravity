<#
.SYNOPSIS
  Downloads and stages the LGPL FFmpeg build into app/desktop/src-tauri/resources/ffmpeg/
  ahead of `tauri build` (Phase 5.2).

.DESCRIPTION
  Source is BtbN/FFmpeg-Builds, not gyan.dev -- see docs/licensing.md for why: gyan.dev's
  "essentials"/"full"/"full-shared" builds are all GPLv3 (they bundle libx264), and
  gyan.dev does not publish a ready-made LGPL artifact at all. BtbN explicitly ships
  separate GPL/LGPL variants for exactly this use case.

  Verifies the downloaded archive's SHA256 against the release's checksums.sha256 before
  extracting anything, and refuses to proceed on a mismatch -- a corrupted or tampered
  download must never silently end up inside a shipped installer. Prints the verified
  hash so it can be recorded in docs/licensing.md's pinning section.

.NOTES
  Windows/PowerShell only, matching the rest of this project's build tooling
  (docs/development.md). Not yet exercised in CI -- see Phase 5.4.
#>

[CmdletBinding()]
param(
    # BtbN publishes releases under a rolling "latest" tag that gets overwritten on every
    # upstream CI run (see docs/licensing.md's pinning note). Pass -ReleaseTag to pin a
    # specific past release instead, once one has actually been chosen for a real shipped
    # build.
    [string]$ReleaseTag = "latest",
    [string]$Variant = "ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$ResourcesDir = Join-Path $RepoRoot "app\desktop\src-tauri\resources\ffmpeg"
$DownloadDir = Join-Path $RepoRoot "build\vendor-cache"
$ArchivePath = Join-Path $DownloadDir $Variant
$ChecksumsPath = Join-Path $DownloadDir "checksums.sha256"

$BaseUrl = "https://github.com/BtbN/FFmpeg-Builds/releases/download/$ReleaseTag"

New-Item -ItemType Directory -Force -Path $DownloadDir | Out-Null
New-Item -ItemType Directory -Force -Path $ResourcesDir | Out-Null

Write-Host "Downloading $Variant from BtbN/FFmpeg-Builds ($ReleaseTag)..."
Invoke-WebRequest -Uri "$BaseUrl/$Variant" -OutFile $ArchivePath
Invoke-WebRequest -Uri "$BaseUrl/checksums.sha256" -OutFile $ChecksumsPath

$ExpectedLine = Select-String -Path $ChecksumsPath -Pattern ([regex]::Escape($Variant)) | Select-Object -First 1
if (-not $ExpectedLine) {
    throw "checksums.sha256 has no entry for $Variant -- refusing to proceed without a hash to verify against."
}
$ExpectedHash = ($ExpectedLine.Line -split '\s+')[0].ToLowerInvariant()
$ActualHash = (Get-FileHash -Path $ArchivePath -Algorithm SHA256).Hash.ToLowerInvariant()

if ($ExpectedHash -ne $ActualHash) {
    throw "SHA256 mismatch for ${Variant}: expected $ExpectedHash, got $ActualHash. Refusing to extract a download that doesn't match its published checksum."
}
Write-Host "SHA256 verified: $ActualHash"
Write-Host "Record this hash (and the exact $ReleaseTag build date) in docs/licensing.md's FFmpeg pinning section."

$ExtractDir = Join-Path $DownloadDir "extracted"
if (Test-Path $ExtractDir) { Remove-Item -Recurse -Force $ExtractDir }
Expand-Archive -Path $ArchivePath -DestinationPath $ExtractDir -Force

# The archive's top-level folder is named after the build (e.g.
# "ffmpeg-n8.1-latest-win64-lgpl-shared-8.1\{bin,lib,include}") -- only bin\ (the
# executables + runtime DLLs) needs to ship; lib\/include\ are development files Gravity
# never links against (docs/licensing.md: shells out to ffmpeg.exe, never links).
$ExtractedRoot = Get-ChildItem -Path $ExtractDir -Directory | Select-Object -First 1
if (-not $ExtractedRoot) {
    throw "Extracted archive at $ExtractDir has no top-level directory -- BtbN's archive layout may have changed."
}
$BinDir = Join-Path $ExtractedRoot.FullName "bin"
if (-not (Test-Path $BinDir)) {
    throw "Expected a bin\ directory at $BinDir but didn't find one -- BtbN's archive layout may have changed."
}

Get-ChildItem -Path $ResourcesDir -Filter "*.exe" -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem -Path $ResourcesDir -Filter "*.dll" -ErrorAction SilentlyContinue | Remove-Item -Force
Copy-Item -Path (Join-Path $BinDir "*") -Destination $ResourcesDir -Recurse -Force

Write-Host "Staged ffmpeg.exe/ffprobe.exe + runtime DLLs into $ResourcesDir"
