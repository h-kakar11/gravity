<#
.SYNOPSIS
  Downloads a redistributable Python (astral-sh/python-build-standalone), pip-installs
  python/downloader/requirements.txt into it, and stages both it and downloader.py at
  app/desktop/src-tauri/resources/python/ ahead of `tauri build` (Phase 5.2).

.DESCRIPTION
  Gravity's downloader (python/downloader/downloader.py, wrapping yt-dlp) needs an
  interpreter bundled into the packaged app rather than relying on a user's own Python
  install -- the installer previously shipped with no working backend at all (audit #7).
  python-build-standalone is purpose-built for exactly this: a self-contained, relocatable
  CPython distribution, unlike a normal python.org installer (which installs into the
  registry/Program Files and isn't meant to be copied elsewhere).

.NOTES
  Windows/PowerShell only. Not yet exercised in CI -- see Phase 5.4.
#>

[CmdletBinding()]
param(
    [string]$PythonVersion = "3.12.8",
    # python-build-standalone tags releases by build date, not just Python version -- pin
    # both so this script is reproducible.
    [string]$ReleaseTag = "20241206",
    [string]$Variant = "cpython-$PythonVersion+$ReleaseTag-x86_64-pc-windows-msvc-shared-install_only.tar.gz"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$ResourcesDir = Join-Path $RepoRoot "app\desktop\src-tauri\resources\python"
$DownloadDir = Join-Path $RepoRoot "build\vendor-cache"
$ArchivePath = Join-Path $DownloadDir $Variant
$RequirementsPath = Join-Path $RepoRoot "python\downloader\requirements.txt"
$DownloaderScript = Join-Path $RepoRoot "python\downloader\downloader.py"

$Url = "https://github.com/astral-sh/python-build-standalone/releases/download/$ReleaseTag/$Variant"

if (-not (Test-Path $RequirementsPath)) {
    throw "Missing $RequirementsPath -- nothing to install into the bundled runtime."
}

New-Item -ItemType Directory -Force -Path $DownloadDir | Out-Null
if (Test-Path $ResourcesDir) { Remove-Item -Recurse -Force $ResourcesDir }
New-Item -ItemType Directory -Force -Path $ResourcesDir | Out-Null

Write-Host "Downloading Python $PythonVersion ($ReleaseTag) from python-build-standalone..."
Invoke-WebRequest -Uri $Url -OutFile $ArchivePath

Write-Host "Extracting..."
# python-build-standalone ships .tar.gz even for the Windows build; tar.exe has shipped
# with Windows since build 17063 (Win10 1809+), so this needs no extra archive tool.
# --strip-components=1 drops the archive's top-level "python/" wrapper directory so
# python.exe lands directly in $ResourcesDir.
tar -xzf $ArchivePath -C $ResourcesDir --strip-components=1
if ($LASTEXITCODE -ne 0) {
    throw "tar extraction failed with exit code $LASTEXITCODE"
}

$PythonExe = Join-Path $ResourcesDir "python.exe"
if (-not (Test-Path $PythonExe)) {
    throw "Expected python.exe at $PythonExe after extraction -- python-build-standalone's archive layout may have changed."
}

Write-Host "Installing requirements.txt into the bundled runtime..."
& $PythonExe -m ensurepip --upgrade
if ($LASTEXITCODE -ne 0) { throw "ensurepip failed with exit code $LASTEXITCODE" }
& $PythonExe -m pip install --no-cache-dir -r $RequirementsPath
if ($LASTEXITCODE -ne 0) { throw "pip install failed with exit code $LASTEXITCODE" }

# The downloader script is Gravity's own source, not a pip package -- vendor it alongside
# the interpreter so MEDIATOOL_DOWNLOADER_SCRIPT (core_bridge.rs) resolves it from the
# same resource_dir() the interpreter itself lives in.
$DownloaderResourceDir = Join-Path $ResourcesDir "downloader"
New-Item -ItemType Directory -Force -Path $DownloaderResourceDir | Out-Null
Copy-Item -Path $DownloaderScript -Destination $DownloaderResourceDir -Force

Write-Host "Staged the Python runtime + downloader.py into $ResourcesDir"
