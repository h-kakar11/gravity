#!/usr/bin/env python3
"""Fails if Gravity's three independent version manifests disagree.

CMakeLists.txt (C++ core), app/desktop/src-tauri/Cargo.toml (the Tauri shell -- also the
version tauri.conf.json inherits, since it deliberately omits its own "version" field), and
app/frontend/package.json (the frontend) are three different ecosystems' package managers
and cannot literally share one version file. This script is the actual verification for
what "one source of truth" means in practice (docs/phase-7.md "Versioning") -- run it before
tagging a release; a bump to one manifest without the other two is a bug this catches.
"""

import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def cmake_version() -> str:
    text = (REPO_ROOT / "CMakeLists.txt").read_text()
    match = re.search(r"project\(\s*Gravity\s+VERSION\s+([0-9.]+)", text)
    if not match:
        raise SystemExit("could not find `project(Gravity VERSION ...)` in CMakeLists.txt")
    return match.group(1)


def cargo_version() -> str:
    text = (REPO_ROOT / "app/desktop/src-tauri/Cargo.toml").read_text()
    match = re.search(r'^version\s*=\s*"([0-9.]+)"', text, re.MULTILINE)
    if not match:
        raise SystemExit("could not find `version = \"...\"` in src-tauri/Cargo.toml")
    return match.group(1)


def frontend_version() -> str:
    data = json.loads((REPO_ROOT / "app/frontend/package.json").read_text())
    return data["version"]


def main() -> int:
    versions = {
        "CMakeLists.txt": cmake_version(),
        "app/desktop/src-tauri/Cargo.toml": cargo_version(),
        "app/frontend/package.json": frontend_version(),
    }
    distinct = set(versions.values())
    for path, version in versions.items():
        print(f"  {version:>10}  {path}")

    if len(distinct) != 1:
        print(f"\nVersion mismatch: {sorted(distinct)}", file=sys.stderr)
        return 1

    print(f"\nAll manifests agree: {distinct.pop()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
