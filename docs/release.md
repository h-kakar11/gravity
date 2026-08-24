# Gravity release notes — v1.0.0-rc1

This is the release-candidate record Phase 10 produces: what was built, how, what was
tested, what is known not to work in this environment, and exactly what remains before this
candidate becomes a final `v1.0.0`. See `docs/phase-10.md` for the full engineering report
this summarizes, `docs/v1-feature-freeze.md` for what v1 does and deliberately doesn't, and
`docs/third-party-licenses.md` for the dependency license inventory.

## Version

**1.0.0**, identical across `CMakeLists.txt`, `app/desktop/src-tauri/Cargo.toml`,
`app/frontend/package.json`, and `app/desktop/package.json` — verified by
`python3 scripts/check_versions.py`. Kept strictly numeric (no `-rc1` suffix in the
manifests themselves) because CMake's `project(VERSION ...)` rejects pre-release
identifiers and `check_versions.py`'s cross-check is numeric-only by design; the
release-candidate designation lives on the git tag (`v1.0.0-rc1`) instead.

The `v1.0.0-rc1` annotated tag was created locally on this branch's final commit but could
not be pushed to `origin` from this session — `git push origin v1.0.0-rc1` returned an
HTTP 403 from the git proxy on every retry, while ordinary branch pushes to this same
remote succeeded throughout the entire session (most recently the commit this tag points
at). That is a permission-scope difference (this session's credential appears able to push
branch refs but not tag refs), not a network failure, so retrying further would not help
and risking a workaround was not appropriate. **Whoever has full push access to this
repository should run `git tag -a v1.0.0-rc1 <commit> -m "..."` (or `git push origin
v1.0.0-rc1` if they pull this exact local tag) to actually publish the tag** — the commit it
should point at is the one this document and `docs/phase-10.md` describe as final.

## Why "rc1" and not a final GA

Everything this environment can build, run, and test is done, green, and re-verified this
phase (see Test results below). What remains needs a Windows machine, which this sandboxed
environment does not have and has not had at any point since Phase 7:

1. Build the actual Windows installer (`tauri build`, NSIS) and confirm it produces a real
   `.exe`/`.msi`.
2. Source the actual FFmpeg (LGPL build recommended, see `docs/third-party-licenses.md`) and
   Python+yt-dlp binaries into `app/desktop/src-tauri/resources/` via
   `scripts/prepare-release-resources.ps1`.
3. Install the built package on a machine with no dev tools, launch it from the Start Menu
   and a desktop shortcut (not from the repository), confirm resource resolution, and run
   the six core user journeys (`docs/phase-9.md` §3) for real.
4. Uninstall, reinstall, and install-over (upgrade) once to confirm user data
   (`%LOCALAPPDATA%\Gravity\`) survives all three.
5. Optionally: a handful of real YouTube downloads, since every download-path test in this
   project runs against a real subprocess speaking the real protocol
   (`tests/e2e/fake_downloader.py`), never a live network — this sandbox's outbound HTTPS to
   `youtube.com`/`google.com` returns a proxy 403, confirmed directly.

Whoever performs those five steps and finds no blocker can retag the same commit (or a small
follow-up) as `v1.0.0` with confidence — everything else about the product is done.

## Build instructions

```bash
# C++ core
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release -DMEDIATOOL_BUILD_TESTS=OFF
cmake --build build/release -j$(nproc)

# Frontend
cd app/frontend && npm install && npm run build && cd ../..

# Tauri shell + installer (Windows target; see docs/development.md for the full
# winget/vcpkg/rustup toolchain this needs on a real Windows machine)
cd app/desktop && npm install
scripts/prepare-release-resources.ps1   # stages mediatool-core + downloader/*.py
# manually place FFmpeg/ffprobe and a Python+yt-dlp runtime in
# app/desktop/src-tauri/resources/ (see resources/README.md)
npm run tauri build
```

This session built and verified the C++ core, frontend, and Rust shell for the host
platform (Linux) — see Test results below — but did not (cannot, without a Windows machine)
run the actual `tauri build` installer step.

## Test results (this exact commit, this session)

| Suite | Result |
|---|---|
| C++ (`ctest`, clean rebuild from scratch) | 363 tests, 348 pass, 15 skipped (Windows-only), 0 fail |
| Python (`unittest`) | 26 pass |
| Frontend (`vitest`) | 75 pass |
| `tsc --noEmit` | clean |
| `vite build` | clean, 17.3 kB CSS / 223 kB JS (67 kB gzipped) |
| `cargo build` / `cargo check` (Tauri shell) | clean |
| E2E: real ffmpeg (`tests/e2e/queue_ffmpeg_e2e.py`) | 83/83 |
| E2E: real downloads/retries/dependencies (`tests/e2e/queue_download_e2e.py`) | 37/37 |
| E2E: IPC fuzzing (`tests/e2e/ipc_fuzz.py`) | 77/77 |
| Real Tauri app launch (Xvfb virtual display) | Home, Download, Convert & Compress, Queue, Settings (incl. About), Developer console — all screenshotted, live self-test job run to completion, no visual defects |
| Process/file leak audit after the full regression | zero leftover processes, zero orphaned `.part`/`.processing` files |

Full detail, including the two real bugs this phase's crash-recovery re-verification found
and fixed, is in `docs/phase-10.md`.

## Artifact

No packaged installer artifact exists from this session — see "Why rc1" above. What exists
and is verified, all release-profile (`-O2`/LTO, not debug), on this session's host
platform (Linux — the actual Windows binaries will differ in size): `app/core/mediatool-core`
(1.7 MB, 1.4 MB stripped), the Tauri shell (`cargo build --release`: 5.0 MB), and the
frontend production bundle (`app/frontend/dist/`, 240 KB total uncompressed, 70 KB
gzipped). None of these numbers include FFmpeg or a Python+yt-dlp runtime, since those are
not bundled in this session (see "Why rc1" above) — the final installed package size is
dominated by whichever FFmpeg build a release engineer sources (typically 80-150 MB for a
full LGPL/GPL Windows build) plus a Python runtime with yt-dlp (~30-50 MB), not by anything
Gravity's own code contributes.

## Dependency versions (this build environment)

| Tool | Version |
|---|---|
| CMake | 3.28.3 |
| Ninja | 1.11.1 |
| GCC | 13.3.0 |
| Python | 3.11.15 |
| Node.js | 22.22.2 |
| npm | 10.9.7 |
| Rust (rustc) | 1.94.1 |
| Cargo | 1.94.1 |
| nlohmann_json | 3.11.3 |

The Windows toolchain this project is actually designed for (MinGW-w64 GCC, rustup's
`x86_64-pc-windows-gnu` target, vcpkg) is documented in `docs/development.md` and was not
available in this Linux-only sandbox — the versions above are what this session's clean
build and regression actually ran against.

## Known limitations

See `docs/phase-10.md` §8 for the full list with evidence. Summary: no Windows installer
has been built or tested, FFmpeg/yt-dlp binaries are not bundled, no live YouTube network
verification was possible, Windows-specific high-DPI rendering is unverified, and an
abrupt-parent-kill orphan-child-process scenario needs Windows-specific process-lifetime
work to close completely (the file-leak half of the same scenario is fixed this phase; the
graceful paths a real user actually triggers were already correct).

## Release checklist

- [x] Clean build from source (C++, Python, frontend, Rust) — zero errors, zero warnings
- [x] Full regression, exact counts recorded, not narrated
- [x] Crash recovery re-verified for every state the spec names, including a real `SIGKILL`
- [x] Two real bugs found by this phase's own testing, fixed, and covered by new tests
- [x] Security review (IPC fuzzing, injection/traversal, secrets, unsafe temp files)
- [x] Third-party license inventory (`docs/third-party-licenses.md`)
- [x] Brand audit (`docs/phase-10.md` §2)
- [x] Git release hygiene (TODO/FIXME/DEBUG/PLACEHOLDER grep, no scratch files, no
      accidentally-tracked build artifacts)
- [x] One consistent version number (`1.0.0`) across all manifests
- [ ] Windows installer built, installed, and uninstalled on a real machine
- [ ] FFmpeg and yt-dlp binaries sourced and bundled
- [ ] Clean-environment (no dev tools) install and launch verified
- [ ] A handful of real network downloads verified

## Final classification

**RELEASE READY as a release candidate (`v1.0.0-rc1`).** Every check this environment is
capable of performing is complete, green, and re-verified against a from-scratch build in
this exact session. The unchecked items above are not defects — they are Windows-machine-
dependent verification steps this sandboxed environment has never had access to, at any
phase, and have been documented as such since Phase 7. A release engineer with a Windows
machine completing those four items, and finding no blocker, can promote this exact commit
to a final `v1.0.0` tag with confidence.
