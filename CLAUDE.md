# Agent instructions for Gravity

Gravity is a Windows-only local-first media downloader/converter/compressor: React/
TypeScript frontend, Tauri (Rust) shell, a C++ core sidecar (`mediatool-core.exe`,
stdio NDJSON), and a Python (yt-dlp) downloader subprocess. Start with
`docs/architecture.md` for how the pieces fit together, `docs/ipc-contract.md` before
touching any cross-process code, and `docs/decisions.md` for why things are the way they
are before "fixing" something that looks wrong but is deliberate.

## Recommended workflow

```text
1. Inspect the repository (relevant docs, existing tests, related code)
2. Create or switch to a task branch
3. Make changes
4. Run the relevant local test suite(s) for what changed
5. Run the full local validation suite: .\scripts\ci-local.ps1
6. Inspect `git diff` before committing
7. Commit with a clear, descriptive message
8. Push the branch to GitHub
9. Report: commit hash, branch name, what was run, and the results
```

**Do not rely on GitHub Actions to validate your work.** `scripts/ci-local.ps1` runs the
same checks CI runs (see `docs/local-ci.md`) directly on your machine, with no GitHub
Actions runner required — this is the primary validation path, not a fallback. GitHub
Actions still exists as a merge-time safety net (PRs into `master`, pushes to `master`)
and for the installer smoke test, but routine development should never be blocked on it
being available or healthy.

If you can't run `ci-local.ps1` or a given stage from your current environment (e.g. no
PowerShell, no Windows, a missing toolchain), say so explicitly and name what's missing —
never claim a check passed without having actually run it. Run whatever subset you
genuinely can (Python and frontend suites are cross-platform), and be precise in your
report about what was and wasn't verified.

## Local validation

```powershell
.\scripts\ci-local.ps1              # everything
.\scripts\ci-local.ps1 -Cpp
.\scripts\ci-local.ps1 -Rust
.\scripts\ci-local.ps1 -Python
.\scripts\ci-local.ps1 -Frontend
.\scripts\ci-local.ps1 -FailFast    # stop at first failure instead of reporting all
```

Full details, prerequisites, and the exact CI-job-to-local-stage mapping: `docs/local-ci.md`.

## Git command reference

Standard git, nothing project-specific — a cheat sheet, not a tutorial:

| Command | Purpose |
|---|---|
| `git status` | What's changed, staged, untracked |
| `git branch` | List local branches |
| `git checkout <branch>` / `git switch <branch>` | Switch branches |
| `git checkout -b <branch>` / `git switch -c <branch>` | Create and switch to a new branch |
| `git add <files>` | Stage changes |
| `git commit -m "..."` | Commit staged changes |
| `git diff` | Unstaged changes |
| `git diff --staged` | Staged changes |
| `git log --oneline` | Recent commit history |
| `git fetch origin <branch>` | Fetch a branch's latest from the remote |
| `git pull origin <branch>` | Fetch and merge a branch from the remote |
| `git push -u origin <branch>` | Push a new local branch to GitHub, tracking it |

The repo's default/only long-lived branch is `master`. Branch, commit, and push freely —
GitHub itself (hosting, branches, PRs) is unaffected by any Actions capacity/billing
restriction; only the CI *runners* are constrained, which is exactly what
`scripts/ci-local.ps1` and the narrowed `.github/workflows/ci.yml` trigger (see
`docs/local-ci.md`) address.

## Constraints that apply to any change here

- Never hide a failing test, mark a failing check as passing, or remove a test because
  it's inconvenient to run in your environment — see `docs/decisions.md` for suites that
  are deliberately awkward on purpose (e.g. the Python suite runs under the ambient
  interpreter with no `yt_dlp` installed, specifically to exercise a fallback path).
- Don't leave build artifacts committed to git (`build/`, `target/`, `node_modules/`,
  `third_party/vcpkg/`, `vcpkg_installed/` are all gitignored — keep it that way).
- Don't rewrite published git history (no force-push over shared branches, no `commit
  --amend` on commits already pushed and shared) unless explicitly asked to.
- Don't downgrade dependencies or change the application architecture to make something
  easier to test — fix the actual problem, or document why it can't be fixed here.
