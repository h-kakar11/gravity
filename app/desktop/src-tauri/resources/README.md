# Bundled resources

This directory is **staged at release-build time, not committed**. Everything in it except
this file is gitignored (`app/desktop/src-tauri/.gitignore`) — none of it is source, and
some of it (FFmpeg, a portable Python) is large, versioned independently of this repo, and
in FFmpeg's case carries its own license file that must ship alongside it (see
`docs/phase-7.md` "License / third-party review").

`tauri build` copies everything under here into the packaged app's resource directory,
preserving this structure (`tauri.conf.json`'s `bundle.resources`). At runtime,
`mediatool-core` and the Rust shell resolve resources relative to that same directory — see
`core/filesystem/ExecutablePath.h` and `docs/phase-7.md` "Resource discovery" for exactly
how. Nothing in the packaged app depends on the repository, a development PATH, or the
process's current working directory.

## Expected layout

```
resources/
  mediatool-core.exe         the C++ core, built from this repo (Release config)
  downloader/
    downloader.py             and everything else under python/downloader/ except .venv/
    ...
  bin/
    ffmpeg.exe                 official static Windows build (see below)
    ffprobe.exe
    LICENSE-ffmpeg.txt          FFmpeg's own license text -- ships alongside the binary
  python/
    python.exe                  a portable/embeddable Python distribution
    ...                          (only if not vendoring yt-dlp as its own standalone .exe)
```

## Preparing this directory for a release build

Run `scripts/prepare-release-resources.ps1` (Windows, PowerShell) from the repository root
after building the C++ core in Release configuration. It copies `mediatool-core.exe` and
the downloader script from their real locations in this repo into place here. It does
**not** fetch FFmpeg or Python — those are third-party binaries this repository does not
vendor (see below) and must be placed by hand, once, by whoever prepares a release:

1. **FFmpeg/ffprobe** — download an official static Windows build (e.g. from
   ffmpeg.org's documented builds page) and place `ffmpeg.exe`/`ffprobe.exe` plus their
   license text under `bin/`. Pin a specific version for a release and record it in
   `docs/release.md`.
2. **Python + yt-dlp** — either an embeddable Python distribution
   (python.org's "embeddable package") with `yt-dlp` and its dependencies installed into
   it under `python/`, or (the smaller, simpler option, tracked as a Phase 7 known
   limitation in `docs/phase-7.md`) yt-dlp's own standalone Windows `.exe`, which bundles
   its own interpreter — this would need `python/downloader/downloader.py`'s subprocess
   invocation adapted to call it directly rather than through a system Python, which is
   deliberately **not** done in this phase (packaging, not a downloader-architecture
   change).

Building the actual installer (`npm run build` in `app/desktop`, i.e. `tauri build`) also
requires a Windows machine or a properly configured Windows cross-compilation toolchain —
neither is available in this repository's Linux development/CI environment. See
`docs/phase-7.md` "Known limitations" for exactly what was and wasn't verified.
