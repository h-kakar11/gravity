# Third-party licenses and dependency inventory

Produced for Phase 10's release-license review. This is an inventory of what Gravity
actually depends on and links against, not a legal opinion — a release engineer shipping
Gravity to real users should have counsel review this before a public release, particularly
the FFmpeg section below.

## Runtime dependencies invoked as separate processes (not linked)

Gravity never links against these; it spawns them as subprocesses and talks to them over
stdio/argv, exactly the same "mere aggregation" relationship as any tool that shells out to
`ffmpeg` on the user's behalf (spec sections 1, 45; `docs/architecture.md`).

- **FFmpeg / ffprobe** — LGPL v2.1+ or GPL v2+ depending on which optional components the
  specific build was compiled with (e.g. `--enable-gpl` for libx264/libx265). Gravity does
  not build FFmpeg itself and does not currently bundle a specific binary (`docs/phase-7.md`
  "Known limitations" — sourcing and packaging the actual FFmpeg binary is still a manual
  release-engineering step). **Recommendation for whoever performs that step**: use an
  LGPL-only build (e.g. the official `https://www.gyan.dev/ffmpeg/builds/` "essentials" or
  "shared" LGPL builds, not "full" GPL builds) unless there is a deliberate decision to
  accept GPL's stronger obligations, and ship FFmpeg's own `LICENSE.md`/`COPYING.LGPLv2.1`
  file alongside the binary in the installer's resources directory so the license travels
  with the binary it covers. Invoking an unmodified FFmpeg binary as a subprocess does not
  by itself impose FFmpeg's license terms on Gravity's own source, but the FFmpeg binary
  itself must still carry its own license and attribution when redistributed.
- **yt-dlp** — Unlicense (public domain equivalent; see
  <https://github.com/yt-dlp/yt-dlp/blob/master/LICENSE>). No copyleft obligations, no
  attribution legally required, though including a copy of its license text alongside the
  bundled Python runtime (`docs/phase-7.md`'s packaging step) is good practice and costs
  nothing.

## C++ core dependencies (linked, via vcpkg)

All permissive; none require source disclosure or trigger any obligation beyond retaining
copyright/license notices, which vcpkg's own `share/<pkg>/copyright` files already carry
into `vcpkg_installed/` for a release engineer to collect.

| Package | License | Role |
|---|---|---|
| nlohmann/json | MIT | JSON parsing/serialization throughout the core |
| spdlog | MIT | Logging |
| reproc / reproc++ | MIT | Cross-platform child-process spawning |
| GoogleTest | BSD-3-Clause | Test-only — not linked into any shipped binary |

## Rust dependencies (Tauri shell, via `cargo metadata`)

430 packages in the full dependency graph as of this phase. License breakdown (full data:
this phase's `cargo metadata --format-version 1` output):

| License | Count |
|---|---|
| MIT OR Apache-2.0 (and equivalent orderings/separators) | ~258 |
| MIT | 100 |
| Zlib OR Apache-2.0 OR MIT | 17 |
| Unicode-3.0 (Unicode table data) | 18 |
| Unlicense OR MIT | 9 |
| MPL-2.0 | 5 |
| BSD-3-Clause (and BSD-3-Clause OR MIT OR Apache-2.0) | 5 |
| ISC, ZLib, ISC/ apache variants, ~10 other permissive one-offs | ~11 |
| (unset — Gravity's own crate) | 1 |

No GPL, AGPL, or SSPL-licensed crate appears anywhere in the graph. The five MPL-2.0 crates
(`cssparser`, `cssparser-macros`, `dtoa-short`, `option-ext`, `selectors` — transitive
dependencies of the platform webview stack's CSS handling) are file-level weak copyleft:
using them unmodified as a compiled dependency, without redistributing their own source
under a different license, is the same well-established arrangement countless commercial
Rust/Tauri applications already ship under and does not require Gravity's own source to be
disclosed.

Direct dependencies declared in `app/desktop/src-tauri/Cargo.toml`: `tauri` (MIT/Apache-2.0
dual), `serde`/`serde_json` (MIT/Apache-2.0 dual), `tokio` (MIT), `log` (MIT/Apache-2.0
dual) — all upstream-published as dual-licensed, the Rust ecosystem's standard convention.

## Frontend dependencies (npm)

179 packages present in `app/frontend/node_modules`, license field read directly from each
package's own `package.json` (no network lookup needed):

| License | Count |
|---|---|
| MIT | 157 |
| Apache-2.0 OR MIT | 4 |
| Apache-2.0 | 5 |
| ISC | 7 |
| BSD-2-Clause | 2 |
| BSD-3-Clause | 2 |
| MIT-0 | 1 |
| CC-BY-4.0 | 1 |

All permissive. Only three packages are actual runtime dependencies that ship in the built
bundle (everything else is a dev-time build/test tool that `vite build` does not emit):
`@tauri-apps/api`, `react`, `react-dom` — all MIT.

## Gravity's own license

No `LICENSE` file exists at the repository root as of this phase. That is a product/business
decision for the project owner, not something this phase decided on their behalf — flagged
here so it is a deliberate choice before a public release, not an oversight.

## What this review did not do

- No legal counsel reviewed this inventory; treat it as an engineering starting point.
- License text collection (copying every dependency's actual `LICENSE`/`COPYING` file into
  a shipped `NOTICES` file in the installer) was not performed — this document identifies
  what exists and what's needed, but assembling the final notices bundle is release-
  packaging work that belongs with the same manual step that sources the actual FFmpeg and
  Python+yt-dlp binaries (`docs/phase-7.md`).
