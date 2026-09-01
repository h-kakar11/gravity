# Licensing

Gravity's own source is closed (no LICENSE file -- see "Gravity's own source license"
below). This document covers the licensing of what Gravity *bundles and ships*: FFmpeg,
yt-dlp, and the vcpkg-managed C++ dependencies. It exists so "no GPL codecs bundled" stays
a verifiable claim against a specific artifact, not a vague "we used the LGPL build"
assertion that quietly drifts as builds get updated.

## FFmpeg

### The compliance basis (read this first)

Gravity never links against FFmpeg. `engines/ffmpeg/FFmpegEngine.cpp` invokes
`ffmpeg.exe`/`ffprobe.exe` as **separate OS processes** through `IProcessRunner`, the same
way every other external tool in this codebase is launched (structured argv, no shell
string) -- see `docs/architecture.md`. Communication is stdin/stdout/argv only. This
exceeds what LGPL already permits (dynamic linking): Gravity doesn't even dynamically link,
so FFmpeg's license terms constrain only the FFmpeg binary itself, never anything in
`core/`, `engines/`, `app/desktop/src-tauri/`, or the frontend.

That said, Gravity still deliberately vendors an **LGPL-licensed** FFmpeg build, not a GPL
one, and never bundles `libx264`/`libx265` (both GPL-only) -- see "Codec strategy" below.
This is a stricter bar than the process-isolation argument alone requires, chosen so the
whole distributed package (Gravity + its bundled FFmpeg) stays under permissive terms
throughout, not just the parts Gravity itself authored.

### Vendored build

- **Source:** [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds) -- **not**
  gyan.dev. This corrects an assumption made earlier in this project's planning: gyan.dev's
  "essentials", "full", and even "full-shared" builds are all **GPLv3** (they include
  `libx264`), and gyan.dev does not publish a ready-made LGPL artifact. BtbN/FFmpeg-Builds
  is a maintained CI pipeline that explicitly ships separate GPL and LGPL variants (static
  and shared) for exactly this reason.
- **Variant to vendor:** `ffmpeg-n8.1-latest-win64-lgpl-shared-8.1.zip` -- FFmpeg **8.1**
  (a numbered release, not the rolling `master` branch build), **LGPL**, **shared**
  (`.dll`s + executables, smaller download than static). win64.
- **Why "shared" and not "static":** irrelevant to the LGPL-vs-GPL question (both variants
  of a given license track are equally compliant) -- shared is simply the smaller download,
  and Gravity ships the `.dll`s alongside `ffmpeg.exe`/`ffprobe.exe` in `bundle.resources`
  regardless (Phase 5.2), so there's no static-linking simplicity to gain from the static
  variant.
- **Pinning:** BtbN publishes builds under a GitHub Release tag literally named `latest`,
  which is overwritten on every upstream CI run -- there is no stable per-build URL to hash
  ahead of time. The actual pin happens in `scripts/vendor_ffmpeg.ps1` (Phase 5.2): that
  script downloads the artifact, computes its SHA256, and **writes the resulting hash into
  this file** as part of vendoring -- the same "record what you actually shipped" pattern
  `Cargo.lock`/`package-lock.json` already follow elsewhere in this repo. Until that script
  has run for a real release build, no SHA256 is recorded here rather than a fabricated
  placeholder value.

  <!-- scripts/vendor_ffmpeg.ps1 fills this in on first real vendoring run: -->
  - Downloaded: _(not yet vendored)_
  - SHA256: _(not yet vendored)_

### Codec strategy (matches `engines/ffmpeg/FFmpegEngine.cpp` / `FFmpegDiscovery.cpp`)

- **`libopenh264`** (BSD-2-Clause, Cisco) is the default bundled H.264 encoder --
  license-clean and included in the LGPL build above.
- **`libx264`/`libx265`** (GPL-2.0-or-later) are **never bundled**. `videoCodec: "auto"`
  only prefers them when `DiscoverAvailableEncoders()` finds them on a binary the *user*
  supplied themselves (a separately-installed ffmpeg pinned via `advanced.ffmpegPath`) --
  at that point they're the user's own GPL obligation to manage, not something Gravity
  distributed.
- **`libvpx`** (VP9, BSD-3-Clause) and **AV1** (`libaom`/`libsvtav1`, BSD-2-Clause/BSD-3-Clause)
  are permissively licensed and fine to bundle; both are present in BtbN's LGPL build.

### Attribution

FFmpeg is a trademark of Fabrice Bellard, originator of the FFmpeg project. This build is
licensed under the [GNU Lesser General Public License version 2.1 or later](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html).
Full FFmpeg source is available from [ffmpeg.org](https://ffmpeg.org) and
[github.com/FFmpeg/FFmpeg](https://github.com/FFmpeg/FFmpeg); the exact build vendored here
is from [github.com/BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds).

## yt-dlp

[yt-dlp](https://github.com/yt-dlp/yt-dlp) is licensed under
[The Unlicense](https://unlicense.org/) -- public domain, no attribution or notice
obligations. Vendored as a pip dependency into the bundled Python runtime (Phase 5.2), not
modified.

## vcpkg (C++) dependencies

See `vcpkg.json` for the authoritative version-pinned list; `THIRD_PARTY_LICENSES.md`
carries the full license text for each. Summary:

| Package | License | Role |
|---|---|---|
| `nlohmann-json` | MIT | JSON parsing throughout `core/` |
| `spdlog` | MIT | Logging (`core/logging/`) |
| `reproc` | MIT | Process spawning (`core/process/RealProcessRunner.cpp`) |
| `gtest` | BSD-3-Clause | Test-only (`tests/`) -- never shipped in the packaged app |

All four are permissive licenses with no copyleft or distribution obligations beyond
attribution, which `THIRD_PARTY_LICENSES.md` provides.

## Rust and npm dependencies

Not individually itemized here -- the Rust crate graph (`tauri` and its official plugins,
`notify`, `cron`, `chrono`, `tokio`, `serde`, `log`) and the npm graph (`react`,
`@tauri-apps/*`, `@fontsource/inter`) are, without exception, MIT and/or Apache-2.0
dual/either-licensed, the Rust and JS ecosystems' near-universal convention for exactly
this reason. `Cargo.lock`/`package-lock.json` (already committed) are the authoritative,
version-pinned record of the full dependency graph if a future audit needs it.

## Gravity's own source license

**Open decision, deliberately left unresolved in this pass** (per the user's own choice --
see `docs/decisions.md` "Phase 5"): no root `LICENSE` file exists yet. The default legal
state of a repository with no LICENSE file is "all rights reserved" (no one may copy,
modify, or redistribute without permission) -- but that's a consequence of the *absence*
of a file, not a considered choice recorded here. (This paragraph previously justified it
by reference to a planned "Pro" commercial tier; issue #82 removed that tier, which leaves
the licensing question genuinely open rather than merely unstated.) Revisit before any public
distribution of source (not just the packaged binary).
