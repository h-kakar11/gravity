# Local CI

`scripts/ci-local.ps1` is the primary way to validate a change to Gravity -- it runs the
same checks `.github/workflows/ci.yml` runs, on your own machine, with no GitHub Actions
runner required.

## Why

GitHub Actions on this repo/account hit a billing/runner-capacity limit: for a stretch of
this project's history, every push failed within a few seconds with no runner ever
assigned (`runner_id: 0`, empty logs) -- not a code problem, an Actions-capacity problem.
Separately, `ci.yml` used to trigger on *every* push to *every* branch, so each routine
commit on a feature/agent branch burned a full remote CI run for a check that also runs
this same session's Python/frontend suites locally already.

Local-first CI fixes both: a developer or coding agent can fully validate a change without
GitHub Actions being reachable or healthy at all, and GitHub Actions itself now only runs
where it earns its cost -- pull requests into `master`, pushes to `master`, and on-demand
via `workflow_dispatch`. See `.github/workflows/ci.yml`'s top-of-file comment for the exact
trigger change and reasoning.

This does **not** mean GitHub Actions was removed. It's still the merge-time safety net and
still hosts the full installer smoke test (`installer-smoke-test` job, manual/nightly-only,
unchanged). Nothing about the CI job definitions themselves changed -- `ci-local.ps1`
mirrors their commands exactly so it can't silently drift from what "passing" means.

## Usage

```powershell
.\scripts\ci-local.ps1              # all 4 stages
.\scripts\ci-local.ps1 -Cpp         # just the C++ build + ctest
.\scripts\ci-local.ps1 -Rust        # just cargo test + clippy
.\scripts\ci-local.ps1 -Python      # just the Python unittest suite
.\scripts\ci-local.ps1 -Frontend    # just tsc + vite build + vitest
.\scripts\ci-local.ps1 -FailFast    # stop at the first failing stage instead of
                                     # running every requested stage and reporting
                                     # all failures together
```

Stage switches can be combined (e.g. `-Python -Frontend` runs just those two). With no
switches, all 4 stages run.

Exit code is `0` only if every requested stage passed; otherwise it's the count of
failed/skipped stages. Each stage prints `[PASS]`/`[FAIL]`/`[SKIP]` with its name and
timing, followed by a summary block -- written to be equally readable by a human skimming
the terminal and an agent parsing the output for a specific stage's result.

## What each stage runs

Exactly what `.github/workflows/ci.yml`'s 4 fast jobs run, so this table is also the CI
job -> local stage mapping:

| Stage | Commands | CI job |
|---|---|---|
| `-Cpp` | `cmake --preset windows-mingw-debug` → `cmake --build --preset windows-mingw-debug` → `ctest --preset windows-mingw-debug --output-on-failure` | `cpp` |
| `-Rust` | (in `app/desktop/src-tauri`) `cargo test` → `cargo clippy --all-targets -- -D warnings` | `rust` |
| `-Python` | `python -m unittest discover -s tests/python` | `python` |
| `-Frontend` | (in `app/frontend`) `npm ci` → `npm run build` (`tsc --noEmit && vite build`) → `npm run test` (`vitest run`) | `frontend` |

The `installer-smoke-test` job (full NSIS build) is intentionally **not** part of
`ci-local.ps1` -- it's slow (vendors ~200 MB of FFmpeg/Python resources and runs a real
`tauri build`) and already gated to manual/nightly in CI for the same reason. To build and
test the installer locally, follow `docs/development.md`'s "Packaging" section directly.

## Prerequisites

`ci-local.ps1` checks for required tools on PATH before running each stage and fails with a
clear message naming what's missing -- it does **not** install anything (no silent
`choco install`, no `rustup default` change) on your machine. Install prerequisites
yourself first; see `docs/development.md`'s Prerequisites table for exact commands
(CMake, Ninja, MinGW-w64 GCC, Rust with the GNU host toolchain + clippy, Python, Node.js).

`third_party/vcpkg` is the one exception: if it's missing, the `-Cpp` stage bootstraps it
automatically (a one-time clone + `bootstrap-vcpkg.bat`, same as CI does on every run) --
locally this only needs to happen once, so it's a genuine convenience rather than a hidden
system-level install.

The Python stage deliberately runs under the **ambient interpreter**, not a venv with
`yt_dlp` installed -- `tests/python/test_downloader_protocol.py` specifically exercises the
"`yt_dlp` is not installed" fallback path for real. See `docs/decisions.md` ("Python test
suite stays unittest, not pytest"). Do not "fix" a local Python stage failure by installing
`yt_dlp` first -- that would be testing a different code path than the one CI verifies.

## What's verified vs. not (as of this doc's introduction)

The Python and frontend command sequences above are cross-platform and have been run
directly (via bash, not through `ci-local.ps1` itself, since this doc was authored from a
Linux sandbox with no PowerShell available) and pass. The `.ps1` script itself, and the
C++/Rust stages (which need MinGW + vcpkg on real Windows), have **not** been executed from
that environment -- the script was written carefully against the known-good commands in
`.github/workflows/ci.yml` and `docs/development.md`, but its first real run should be
treated as the first real verification. If it doesn't work exactly as documented here on a
real Windows machine, that's a bug in the script, not in this doc's intent -- please file an
issue or fix it forward.

## Local git hosting (Gitea) -- considered, not adopted

GitHub Actions being billing-restricted only affects *runners* -- GitHub itself is still
fully reachable for `git push`/`pull`/branches/PRs. Normal local git plus the existing
GitHub remote is sufficient for the full agent/developer workflow (see `CLAUDE.md`); there
is no capability gap a local Gitea instance would actually close right now, only an extra
service to run and maintain. Noted here as a possible future option only if GitHub *hosting
itself* (not just Actions) ever becomes unavailable -- not installed, not a dependency
today.
