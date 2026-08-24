# Development guide

## Prerequisites

This project deliberately avoids requiring Visual Studio / MSVC Build Tools (a multi-GB,
admin-elevated install). The supported Phase 1 toolchain on Windows is:

| Tool | Install | Why |
|---|---|---|
| Git | `winget install Git.Git` | already assumed present |
| Node.js (LTS) | `winget install OpenJS.NodeJS.LTS` | frontend + Tauri CLI |
| Python 3.x | `winget install Python.Python.3.13` | downloader subsystem |
| CMake | `winget install Kitware.CMake` | C++ build |
| Ninja | `winget install Ninja-build.Ninja` | C++ build |
| MinGW-w64 GCC (UCRT, POSIX threads) | `winget install BrechtSanders.WinLibs.POSIX.UCRT` | C++ compiler (no MSVC needed) |
| Rust (GNU host) | `winget install Rustlang.Rustup`, then `rustup toolchain install stable-x86_64-pc-windows-gnu && rustup default stable-x86_64-pc-windows-gnu` | Tauri shell, without needing MSVC's linker |
| FFmpeg | `winget install Gyan.FFmpeg` | optional — the app works without it, but conversion/compression need it later |

After any `winget install`, PATH is updated in the registry but **not** in your current
shell — open a new terminal (or refresh `$env:Path` from the registry) before using a
newly installed tool.

vcpkg is vendored as a git clone at `third_party/vcpkg/` (gitignored — clone it yourself,
it is not committed):

```bash
git clone https://github.com/microsoft/vcpkg.git third_party/vcpkg
./third_party/vcpkg/bootstrap-vcpkg.bat -disableMetrics
```

## Building the C++ core

```bash
cmake --preset windows-mingw-debug
cmake --build --preset windows-mingw-debug
ctest --preset windows-mingw-debug
```

The first configure triggers vcpkg to build `nlohmann-json`, `spdlog`, `reproc`/`reproc++`,
and `gtest` for the `x64-mingw-static` triplet (a few minutes, cached afterward under
`vcpkg_installed/`, also gitignored). Output binaries land in
`build/windows-mingw-debug/`, notably `app/core/mediatool-core.exe` — the sidecar process
the Tauri shell spawns (see `docs/ipc-contract.md`).

Why MinGW instead of MSVC: this keeps the entire toolchain installable per-user via
winget/rustup with no admin-elevated Visual Studio installer and no multi-GB download.
`mediatool-core.exe` links its C++ runtime statically (`-static -static-libgcc
-static-libstdc++`, see the top-level `CMakeLists.txt`) so it runs standalone.

## Running the Python downloader subsystem standalone

```bash
python -m venv python/downloader/.venv
python/downloader/.venv/Scripts/pip install -r python/downloader/requirements.txt
python/downloader/.venv/Scripts/python python/downloader/downloader.py --selftest
```

`--selftest` emits the NDJSON event protocol from `docs/ipc-contract.md` with no network
access — use it to verify the protocol without hitting a real URL.

## Running the desktop app

```bash
cd app/frontend && npm install && cd ../..
cd app/desktop && npm install && npm run tauri dev
```

The Tauri shell spawns `mediatool-core.exe` (built above) as a subprocess and bridges its
stdio NDJSON protocol to the frontend as Tauri commands/events — see
`docs/architecture.md` and `docs/ipc-contract.md`.

**If the app panics on launch** with `failed to spawn mediatool-core ... The system cannot
find the path specified`: `core_bridge.rs`'s dev-mode sidecar path is a relative guess
(`../../build/windows-mingw-debug/app/core/mediatool-core.exe`) built on the assumption
that `cargo run`'s working directory is `app/desktop` — true for some Tauri CLI
versions/invocations, not others (observed during Phase 2 testing: the same command that
worked in Phase 1 later ran `cargo` from `app/desktop/src-tauri` instead, one directory
level off). Set these env vars to absolute paths before `npm run tauri dev` to bypass the
guess entirely:

```powershell
$env:MEDIATOOL_CORE_PATH = "A:\path\to\gravity\build\windows-mingw-debug\app\core\mediatool-core.exe"
$env:MEDIATOOL_PYTHON_PATH = "A:\path\to\gravity\python\downloader\.venv\Scripts\python.exe"
$env:MEDIATOOL_DOWNLOADER_SCRIPT = "A:\path\to\gravity\python\downloader\downloader.py"
```

The latter two matter because `mediatool-core.exe` inherits its own working directory
from whatever spawned it (the Tauri Rust process), so the same CWD mismatch can affect its
*own* relative defaults for finding the Python venv, not just Rust's guess at finding
`mediatool-core.exe` itself.

## One-shot dev bootstrap

There is no single command that does all of the above yet (Phase 2 candidate: a
`scripts/dev.ps1`) — run the C++ build, then the frontend, then the desktop shell, in that
order, each time you pull changes that touch more than the frontend.

## Testing

| Suite | Command |
|---|---|
| C++ (GoogleTest) | `ctest --preset windows-mingw-debug --output-on-failure` |
| Python (`unittest`) | `python -m unittest discover -s tests/python` (deliberately run under the **ambient** interpreter, not the venv — see the module docstring in `tests/python/test_downloader_protocol.py`) |
| Frontend (`vitest`) | `cd app/frontend && npm test` |
| End-to-end queue, FFmpeg | `python tests/e2e/queue_ffmpeg_e2e.py` (needs `ffmpeg`/`ffprobe`, no network) |
| End-to-end queue, downloads + retry | `python tests/e2e/queue_download_e2e.py` (no network) |

The two end-to-end suites drive the **real** `mediatool-core` binary over its real NDJSON
protocol with real FFmpeg and real files. They exist because unit tests cannot demonstrate
the things the queue is actually about: a mocked process runner will happily agree that
concurrency is capped at two, but only counting real `ffmpeg` children proves it. Both of
the bugs that mattered most in Phase 5 — concurrent jobs overwriting each other's output,
and event sequence numbers reaching the wire out of order — were found there and were
invisible to the unit suite. See `tests/e2e/README.md`.

Manual (non-automated, real network) downloader integration test:
`docs/protocols/downloader.md`'s "Manual integration test" section. This is still the only
thing that exercises yt-dlp's own format selection and merge behaviour — the end-to-end
suite deliberately stands yt-dlp in so retry testing can be reproducible and offline.

### Running the C++ suite off Windows

The core builds and its tests run on Linux as well as on the Windows target. Fifteen tests
assert genuinely Windows-specific behaviour (backslash separators, drive-letter roots,
`cmd.exe`) and report as **SKIPPED** rather than failed there, via
`tests/support/PlatformTest.h`. A clean Linux run is therefore "N passed, 15 skipped, 0
failed" — anything else is a real regression.

Do not reach for `SKIP_UNLESS_WINDOWS()` to quiet a test that fails for a
platform-independent reason.

## Building a release / installer

See `docs/phase-7.md` for the full account, including what was and wasn't verified in this
project's own (Windows-less) development environment. Summary, on Windows:

```powershell
cmake --preset windows-mingw-release
cmake --build --preset windows-mingw-release
./scripts/prepare-release-resources.ps1     # stages this repo's own build outputs; see
                                             # app/desktop/src-tauri/resources/README.md
                                             # for the FFmpeg/Python steps it does NOT do
cd app/frontend && npm install && npm run build
cd ../desktop && npm install && npm run build   # tauri build -- produces the NSIS installer
python3 scripts/check_versions.py               # before tagging a release
```

Output: `app/desktop/src-tauri/target/release/bundle/nsis/Gravity_<version>_x64-setup.exe`.

## Why some choices were made

- **GLOB-based CMake source lists** (`file(GLOB_RECURSE ... CONFIGURE_DEPENDS ...)`):
  deliberate for Phase 1 velocity while the file layout is still settling. `CONFIGURE_DEPENDS`
  means CMake reconfigures automatically when a file is added, so this doesn't go stale
  silently. Revisit with explicit source lists if the team grows or build reproducibility
  across CI matters more than convenience.
- **One CMake target per top-level directory** (`mtcore`, `mtengines`) rather than one per
  submodule (jobs/, events/, filesystem/, ...): spec section 41 explicitly warns against
  overengineering Phase 1's architecture. Split further only when a real need appears
  (e.g. an engine needing to be optionally excluded from the build).
- **A sidecar process over stdio NDJSON, not a Rust↔C++ FFI boundary**: avoids `cxx`/`bindgen`
  build complexity and ABI concerns for Phase 1, at the cost of one process-spawn and a
  JSON-parsing hop. Matches the same "controlled process abstraction" philosophy already
  used for FFmpeg and Python (spec section 2). Revisit if the process-spawn overhead ever
  matters, which is unlikely for a tool whose operations are measured in seconds.
