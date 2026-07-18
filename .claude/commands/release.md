Operate the three-tier release process: instant builds, nightlies, and stable releases.

## Versioning

One version number everywhere: git tag `vX.Y.Z` == extension version == release
page. Odd minor = pre-release channel, even minor = stable (the VS Code
Marketplace convention). Nightlies compute `X.<odd>.YYMMDDHH` (UTC hour) on the
odd minor above the newest release — `0.1.*` today, `0.3.*` after stable
`v0.2.0` — so nobody edits version numbers by hand. The versions in `CMakeLists.txt`, `pixi.toml`,
and `editors/vscode/package.json` are permanent placeholders (`0.1.0`); the
real version is injected from the tag at build time (binary via git describe,
vsix via CI). A local `vsce publish` with the placeholder is rejected by the
Marketplace — that is intentional accident protection.

## Tier 1 — Instant builds (every green CI run)

Nothing to operate. Every `main` push and PR run repackages the test-suite
binaries (no LTO, no strip) into `vsix-build-<target>` workflow artifacts, and
the raw binaries are in `native-build-*` / `cross-build-*` artifacts. To hand a
fix to a user: point them at the run's artifact (GitHub login required), or
have them set `clice.executable` to the extracted binary.

## Tier 2 — Nightly (pre-release channel)

`nightly.yml` runs daily (cron) or via `gh workflow run nightly.yml`:
skips when main has no new commits, otherwise tags the nightly version, builds the
full release matrix (ICF + strip + GSYM symbols), publishes a GitHub
pre-release and Marketplace `--pre-release`, and prunes odd-minor pre-releases
older than 30 days. A failed nightly just means no nightly that day — fix main
and rerun via dispatch.

## Tier 3 — Stable release (manual, even minor)

1. Freeze: stop merging features; the last nightlies of the freeze are the RCs.
2. When a nightly is judged good, tag its commit: `git tag v0.2.0 <commit> && git push origin v0.2.0`.
   The tag push runs the release path of `main.yml` (packages + extensions,
   Marketplace without `--pre-release`).
3. Write the release notes on the GitHub release page (the download table
   format from the nightly notes is a good template).
4. Verify: assets present (6 packages + 4 symbol archives + 2 PDB zips +
   6 vsix), Marketplace shows the new stable version.

## Dry run / plumbing changes

PRs touching release plumbing (publish workflows, `cmake/release.cmake`,
`cmake/archive.cmake`, `CMakeLists.txt`) automatically run the full 6+6 dry
run via the `release-check-*` jobs; `gh workflow run main.yml --ref <branch>`
does the same on demand. Uploads stay tag-gated, so nothing publishes.

## Crash log support

Ask the user for the log (worker `.log` from the workspace `.clice/logs/` or
the cache dir). The crash section starts with `clice <version> <target>` —
download that release's `*-symbol.tar.xz` (GSYM) and run:

```bash
python scripts/symbolize.py crash.log --symbols clice.gsym
```

For core-dump-level debugging, fetch the full DWARF from the release run's
`debug-info-*` workflow artifact (90-day retention). If the log predates the
version line or the release was pruned, symbolization is not possible — ask
the user to reproduce on a current nightly.
