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

## Packaging (Phase 5.2)

A packaged build needs `app/desktop/src-tauri/resources/` populated before building -- it's
gitignored (never committed; see `docs/licensing.md` for what's vendored and from where):

```powershell
cmake --build --preset windows-mingw-release
cargo build --release --manifest-path app\desktop\src-tauri\Cargo.toml
.\scripts\prepare_bundle_resources.ps1
cd app\desktop
npm run tauri build -- --config ..\..\app\desktop\src-tauri\tauri.release.conf.json
```

`prepare_bundle_resources.ps1` copies the freshly-built `mediatool-core.exe` and
`WebView2Loader.dll`, and calls `scripts/vendor_ffmpeg.ps1` (LGPL FFmpeg, see
`docs/licensing.md`) and `scripts/vendor_python_runtime.ps1` (a redistributable Python +
`pip install`ed yt-dlp) -- each also runs standalone if only one needs refreshing.

**The preliminary `cargo build --release`**: Tauri's own bundler auto-includes
`WebView2Loader.dll` for the MSVC target, but not for the GNU/MinGW target this project
uses -- without it, the packaged app fails at launch with "WebView2Loader.dll was not
found." `prepare_bundle_resources.ps1` bundles it explicitly (same as `mediatool-core.exe`),
but that requires it to already exist in `target\release\`, which only happens once the
Tauri/Rust side has been compiled at least once -- hence this step before
`prepare_bundle_resources.ps1`. The subsequent `npm run tauri build` reuses cargo's cache
rather than rebuilding from scratch.

**Why `--config tauri.release.conf.json` instead of putting `bundle.resources` straight
into `tauri.conf.json`:** Tauri's build script validates every configured resource path
exists at *compile* time, not just at packaging time -- unconditionally, on every `cargo
check`/`cargo build`/`npm run tauri dev`, whether or not you're actually bundling anything.
Baking `bundle.resources` into the base config would mean nobody could even open this
project and run `npm run tauri dev` without first vendoring 200+ MB of FFmpeg/Python
binaries. `tauri.release.conf.json` holds only the `bundle.resources` addition and is
merged in (Tauri CLI's `--config`, a JSON Merge Patch over the base config) exclusively for
the one command that actually needs it -- dev mode and plain `cargo` commands never see it,
so `resources/` not existing yet is a non-issue for everyday development.

## Testing

| Suite | Command |
|---|---|
| C++ (GoogleTest) | `ctest --preset windows-mingw-debug --output-on-failure` |
| Python (`unittest`) | `python -m unittest discover -s tests/python` (deliberately run under the **ambient** interpreter, not the venv — see the module docstring in `tests/python/test_downloader_protocol.py`) |

Manual (non-automated, real network) downloader integration test:
`docs/protocols/downloader.md`'s "Manual integration test" section.

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
