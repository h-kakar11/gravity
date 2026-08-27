<#
.SYNOPSIS
    Single local validation entry point for Gravity -- mirrors the 4 fast jobs in
    .github/workflows/ci.yml (cpp, python, rust, frontend) command-for-command so this
    script can never silently drift from what CI defines as "passing."

.DESCRIPTION
    Run with no switches to run all 4 stages. Pass one or more stage switches to run only
    those stages. Each stage's commands are the same ones CI runs (see ci.yml) -- this
    script does NOT reimplement or "improve" them, it just runs them locally without
    needing a GitHub Actions runner.

    This script checks for required tools on PATH and fails fast with a clear message if
    something is missing -- it does not install system packages (mingw, rust, node, etc.)
    on your machine. See docs/development.md's Prerequisites table for install commands,
    and docs/local-ci.md for the full explanation of this script's design.

    By default every requested stage runs even if an earlier one fails, so a single
    invocation reports every failure at once. Pass -FailFast to stop at the first failure.

.PARAMETER Cpp
    Run the C++ build + ctest stage.
.PARAMETER Rust
    Run the Rust cargo test + clippy stage.
.PARAMETER Python
    Run the Python unittest stage.
.PARAMETER Frontend
    Run the frontend tsc + vite build + vitest stage.
.PARAMETER FailFast
    Stop at the first stage failure instead of running every requested stage and
    reporting all failures together.

.EXAMPLE
    .\scripts\ci-local.ps1
    Runs all 4 stages.

.EXAMPLE
    .\scripts\ci-local.ps1 -Cpp -Rust
    Runs only the C++ and Rust stages.

.EXAMPLE
    .\scripts\ci-local.ps1 -Frontend -FailFast
    Runs only the frontend stage; irrelevant here with a single stage, but consistent
    with combining -FailFast with any subset.
#>

[CmdletBinding()]
param(
    [switch]$Cpp,
    [switch]$Rust,
    [switch]$Python,
    [switch]$Frontend,
    [switch]$FailFast
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

# No stage switch given -> run everything, matching `.\scripts\ci-local.ps1` with no args
# being "run the full suite" per the task spec.
$runAll = -not ($Cpp -or $Rust -or $Python -or $Frontend)
if ($runAll) {
    $Cpp = $true
    $Rust = $true
    $Python = $true
    $Frontend = $true
}

$results = [ordered]@{}
$stopwatch = [System.Diagnostics.Stopwatch]::new()

function Write-Stage-Header([string]$Name) {
    Write-Host ""
    Write-Host "==> $Name" -ForegroundColor Cyan
}

function Test-CommandExists([string]$Name) {
    return [bool](Get-Command $Name -ErrorAction SilentlyContinue)
}

function Invoke-Stage {
    param(
        [string]$Name,
        [scriptblock]$Prereqs,
        [scriptblock]$Run
    )

    Write-Stage-Header $Name
    $stopwatch.Restart()

    try {
        & $Prereqs
    } catch {
        Write-Host "[SKIP] $Name -- missing prerequisite: $($_.Exception.Message)" -ForegroundColor Yellow
        $results[$Name] = "SKIP"
        if ($FailFast) { Show-Summary; exit 1 }
        return
    }

    try {
        & $Run
        if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) {
            throw "stage exited with code $LASTEXITCODE"
        }
        $stopwatch.Stop()
        Write-Host "[PASS] $Name ($([math]::Round($stopwatch.Elapsed.TotalSeconds, 1))s)" -ForegroundColor Green
        $results[$Name] = "PASS"
    } catch {
        $stopwatch.Stop()
        Write-Host "[FAIL] $Name ($([math]::Round($stopwatch.Elapsed.TotalSeconds, 1))s) -- $($_.Exception.Message)" -ForegroundColor Red
        $results[$Name] = "FAIL"
        if ($FailFast) { Show-Summary; exit 1 }
    }
}

function Show-Summary {
    Write-Host ""
    Write-Host "==> Summary" -ForegroundColor Cyan
    $failCount = 0
    foreach ($key in $results.Keys) {
        $status = $results[$key]
        $color = switch ($status) {
            "PASS" { "Green" }
            "FAIL" { "Red"; $failCount++ }
            "SKIP" { "Yellow"; $failCount++ }
            default { "White" }
        }
        Write-Host ("  [{0}] {1}" -f $status, $key) -ForegroundColor $color
    }
    Write-Host ""
    if ($failCount -eq 0) {
        Write-Host "All requested stages passed." -ForegroundColor Green
    } else {
        Write-Host "$failCount stage(s) failed or were skipped." -ForegroundColor Red
    }
    return $failCount
}

# ---------------------------------------------------------------------------
# C++: cmake --preset windows-mingw-debug -> cmake --build -> ctest
# ---------------------------------------------------------------------------
if ($Cpp) {
    Invoke-Stage -Name "C++" -Prereqs {
        foreach ($tool in @("cmake", "ninja", "gcc", "g++", "ctest")) {
            if (-not (Test-CommandExists $tool)) {
                throw "'$tool' not found on PATH. See docs/development.md's Prerequisites table (MinGW-w64 GCC, CMake, Ninja)."
            }
        }
        $vcpkgExe = Join-Path $RepoRoot "third_party/vcpkg/vcpkg.exe"
        if (-not (Test-Path $vcpkgExe)) {
            Write-Host "vcpkg not found at third_party/vcpkg -- bootstrapping (one-time)..." -ForegroundColor Yellow
            Push-Location $RepoRoot
            try {
                if (-not (Test-Path (Join-Path $RepoRoot "third_party/vcpkg/.git"))) {
                    git clone https://github.com/microsoft/vcpkg.git third_party/vcpkg
                }
                & "third_party\vcpkg\bootstrap-vcpkg.bat" -disableMetrics
            } finally {
                Pop-Location
            }
        }
    } -Run {
        Push-Location $RepoRoot
        try {
            cmake --preset windows-mingw-debug
            if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
            cmake --build --preset windows-mingw-debug
            if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }
            ctest --preset windows-mingw-debug --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "ctest failed" }
        } finally {
            Pop-Location
        }
    }
}

# ---------------------------------------------------------------------------
# Rust: cargo test -> cargo clippy (in app/desktop/src-tauri)
# ---------------------------------------------------------------------------
if ($Rust) {
    $rustDir = Join-Path $RepoRoot "app/desktop/src-tauri"
    Invoke-Stage -Name "Rust" -Prereqs {
        if (-not (Test-CommandExists "cargo")) {
            throw "'cargo' not found on PATH. See docs/development.md's Prerequisites table (Rust, GNU host toolchain)."
        }
        $installed = rustup target list --installed 2>$null
        if ($installed -notmatch "x86_64-pc-windows-gnu") {
            throw "GNU Rust toolchain not installed. Run: rustup toolchain install stable-x86_64-pc-windows-gnu && rustup default stable-x86_64-pc-windows-gnu"
        }
        if (-not (Test-CommandExists "cargo-clippy")) {
            throw "clippy component not installed. Run: rustup component add clippy"
        }
    } -Run {
        Push-Location $rustDir
        try {
            cargo test
            if ($LASTEXITCODE -ne 0) { throw "cargo test failed" }
            cargo clippy --all-targets -- -D warnings
            if ($LASTEXITCODE -ne 0) { throw "cargo clippy failed" }
        } finally {
            Pop-Location
        }
    }
}

# ---------------------------------------------------------------------------
# Python: unittest discovery, deliberately the ambient interpreter (no venv, no
# yt_dlp installed) -- see docs/decisions.md "Python test suite stays unittest, not
# pytest". Do NOT "fix" this by installing yt_dlp first.
# ---------------------------------------------------------------------------
if ($Python) {
    Invoke-Stage -Name "Python" -Prereqs {
        $pythonCmd = if (Test-CommandExists "python") { "python" } elseif (Test-CommandExists "python3") { "python3" } else { $null }
        if (-not $pythonCmd) {
            throw "'python'/'python3' not found on PATH. See docs/development.md's Prerequisites table."
        }
    } -Run {
        Push-Location $RepoRoot
        try {
            $pythonCmd = if (Test-CommandExists "python") { "python" } else { "python3" }
            & $pythonCmd -m unittest discover -s tests/python
            if ($LASTEXITCODE -ne 0) { throw "python unittest failed" }
        } finally {
            Pop-Location
        }
    }
}

# ---------------------------------------------------------------------------
# Frontend: npm ci -> npm run build (tsc --noEmit && vite build) -> npm run test (vitest)
# ---------------------------------------------------------------------------
if ($Frontend) {
    $frontendDir = Join-Path $RepoRoot "app/frontend"
    Invoke-Stage -Name "Frontend" -Prereqs {
        foreach ($tool in @("node", "npm")) {
            if (-not (Test-CommandExists $tool)) {
                throw "'$tool' not found on PATH. See docs/development.md's Prerequisites table."
            }
        }
    } -Run {
        Push-Location $frontendDir
        try {
            if (Test-Path "package-lock.json") {
                npm ci
            } else {
                npm install
            }
            if ($LASTEXITCODE -ne 0) { throw "npm install failed" }
            npm run build
            if ($LASTEXITCODE -ne 0) { throw "npm run build failed" }
            npm run test
            if ($LASTEXITCODE -ne 0) { throw "npm run test failed" }
        } finally {
            Pop-Location
        }
    }
}

$failCount = Show-Summary
exit $failCount
