---
name: release
description: Operate the three-tier release process — instant builds, nightlies, stable releases — plus crash-log symbolization. Read before touching anything release-related. Hard rule — tags and manual releases need explicit maintainer approval first.
---

Operate the three-tier release process: instant builds, nightlies, and stable releases.

## Versioning

One version number everywhere: git tag `vX.Y.Z` == extension version == release
page. Odd minor = pre-release channel, even minor = stable (the VS Code
Marketplace convention). Nightlies compute `X.<odd>.YYYYMMDDHH` (UTC hour) on the
odd minor above the newest release — `0.1.*` today, `0.3.*` after stable
`v0.2.0` — so nobody edits version numbers by hand. The versions in
`bazel/workspace_status.mjs`, `pixi.toml`, and `editors/vscode/package.json`
are permanent placeholders (`0.1.0`); the real version is injected from the
tag at build time (binary via git describe, vsix via CI). A local `vsce publish` with the placeholder is rejected by the
Marketplace — that is intentional accident protection. Open VSX has no such
guard: a local `pixi run publish-ovsx` publishes the placeholder version for
real, so leave Open VSX to CI.

## Tier 1 — Instant builds (every green CI run)

Nothing to operate. Every `main` push and PR run repackages the test-suite
binaries (no strip) into `vsix-build-<target>` workflow artifacts, and
the raw binaries are in `build-<triple>-<type>` artifacts. To hand a
fix to a user: point them at the run's artifact (GitHub login required), or
have them set `clice.executable` to the extracted binary.

## Tier 2 — Nightly (pre-release channel)

`nightly.yml` runs daily (cron) or via `gh workflow run nightly.yml`:
skips when main has no new commits, otherwise tags the nightly version and
**promotes** — nothing is rebuilt. Every main CI run already packages the
binaries its test suites validated (the build jobs build the packages);
nightly picks the newest main commit whose green run still has live
package artifacts (path-filtered runs such as docs-only commits build
nothing), tags exactly that commit, downloads its packages, attaches them
to a GitHub pre-release plus the Marketplace and Open VSX pre-release
channels, and
prunes odd-minor pre-releases older than 30 days. Promotion fails loudly
if no packaged green run exists. A failed nightly just means no nightly
that day — fix main and rerun via dispatch.

The vsix packaging matrix runs in parallel, but release upload and
registry publishing happen in one serial job (`publish-release`) with
5 attempts per vsix — concurrent publishes trip Marketplace internal
errors (TF400898). Open VSX (org secret `UPLOAD_VSX_EXT`, namespace
`clice-io`) publishes the same vsix after the Marketplace step and runs
even when that step failed, so neither registry blocks the other; it
checks the token with `ovsx verify-pat` first and is skipped with a
notice when the secret is empty. The job is rerun-safe: `gh run rerun <id> --failed`
re-uploads with `--clobber` and skips already-published versions, so a
transient registry failure is recovered by rerunning just that job —
never by a new tag.

The binary embeds its build identity (`git describe`: nearest tag + commit
hash), not the release tag — the tag is applied after the build. Match crash
logs to releases by the commit hash; the release notes state the hash.

## Tier 3 — Stable release (manual, even minor)

1. Freeze: stop merging features; the last nightlies of the freeze are the RCs.
2. When a nightly is judged good, tag its commit: `git tag v0.2.0 <commit> && git push origin v0.2.0`.
   The tag push runs the release path of `main.yml`, which promotes the
   already-tested packages from that commit's green CI run and publishes the
   extensions (Marketplace and Open VSX, without the pre-release flag). The promote path
   creates the GitHub release if the tag push did not. Package artifacts
   expire 30 days after the CI run, so the tagged commit's run must be
   less than a month old — for an older commit, rerun its main workflow
   first to regenerate the packages.
3. Write the release notes on the GitHub release page (the download table
   format from the nightly notes is a good template).
4. Verify: assets present (6 packages + 6 symbol archives + 6 vsix),
   Marketplace and Open VSX show the new stable version.

## Plumbing changes

There is no separate dry run: packaging runs on every CI build (the
packaged legs of `test.yml` build `//:package` and `//:symbols`) and the vsix
path runs as `instant-vscode`, so release plumbing is exercised by every
code-touching PR. Only the promote/upload glue (`publish-clice.yml`,
nightly orchestration) is release-time-only.

## Crash log support

Ask the user for the log (worker `.log` from the session log directory —
printed at startup in the editor's clice output panel, by default
`~/.cache/clice/<workspace>-<hash>/logs/<session>/`, falling back to the
workspace `.clice/logs/` when no home directory is available). The crash section starts with `clice <version> <target>` —
download that release's `*.symbols.tar.xz` (`.zip` for Windows; GSYM) and run:

```bash
python scripts/symbolize.py crash.log --symbols clice.gsym
```

For core-dump-level debugging, fetch the full DWARF from the `debug-info-*`
artifact of the main CI run that built the release (90-day retention; find
it via the commit hash in the release notes). If the log predates the
version line or the release was pruned, symbolization is not possible — ask
the user to reproduce on a current nightly.
