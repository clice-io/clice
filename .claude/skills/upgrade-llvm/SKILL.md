---
name: upgrade-llvm
description: Read before upgrading xclang/LLVM (arg = the xclang version, e.g. 23.1.2.1), touching MODULE.bazel, .bazelrc, BUILD.bazel or bazel/, or regenerating modules/ — after a library bump, or when clice needs a header or macro it does not export.
---

Upgrade LLVM to a new version. Accepts the xclang version as argument (e.g., `23.1.2.1`: LLVM 23.1.2, xclang's first build of it).

The compiler clice builds with and the LLVM/Clang libraries it links both come from one [xclang](https://github.com/clice-io/xclang) release: xclang's Bazel module (registry https://bazel.clice.io) brings the toolchain and the release's `libclang-<version>-<triple>[-asan].tar.xz` archives as repositories, and the conda package `xclang` (channel `https://conda.clice.io`) the LLVM tools the tests and the packaging run. They always move together: the archives hold ThinLTO bitcode, which only the LLVM that wrote it is sure to read. clice imports libclang's headers, with every other third-party library's, through the generated module in `modules/`, so an upgrade regenerates it before anything builds. Follow each step in order.

**Read `toolchain-changelog.md` in this directory before touching `MODULE.bazel`, `.bazelrc`, `BUILD.bazel` or `bazel/`**: it records the platform pitfalls that still apply (symptom, cause, fix, how to check). Every new one is added there in the same shape; a toolchain change without an entry is not finished.

The work happens on a branch `chore/upgrade-llvm-<major>`, created from fresh `origin/main` (CLAUDE.md) before Step 2, and runs on two boxes (the agent-box skill), leased once and kept for the whole upgrade: a Linux one, which also cross-builds arm64 Linux and both Windows targets as CI does, and a macOS one, which builds its own and x64 macOS.

## Step 1: The xclang Release

The toolchain and the libraries are built in the xclang repository, by its own pipeline; a new LLVM version there is a change of its `LLVM_VERSION` and sources, and ends with a release tagged `<llvm version>.<revision>`, the conda packages on conda.clice.io and the Bazel module on bazel.clice.io. Upgrading clice starts once that release exists:

```bash
gh release view <VERSION> -R clice-io/xclang --json assets --jq '.assets[].name' | grep libclang
```

Every target clice builds needs its `libclang-<VERSION>-<triple>.tar.xz`, and the Debug legs (x86_64 Linux, arm64 macOS) their `-asan` archives.

## Step 2: A clice to Regenerate With

On both boxes, build clice from this tree before any pin moves, and keep it aside:

```bash
pixi run build RelWithDebInfo
rm -rf ~/clice-bin && cp -RL build/RelWithDebInfo/bin ~/clice-bin
```

The new headers need not build the old `modules/` (a name it exports may be gone upstream), and regenerating only reads them. But the clice must carry this tree's own `clice modularize`: one built from an older tree writes another shape (thousands of extra lines of enum wrappers). main's CI build artifact (`build-<triple>-RelWithDebInfo`, kept 3 days) serves as well: unpacked into `~/clice-bin`, it needs `chmod +x ~/clice-bin/bin/*` (artifacts drop the executable bit), and a box that skips the build needs `pixi run install-tests` before Step 4, or `npx bazel` gets a 404.

## Step 3: Pin It

Move every pin in one change:

- `MODULE.bazel`: `bazel_dep(name = "xclang", version = "<VERSION>")`
- `pixi.toml`: every `xclang = "==<VERSION>"`, and the Windows test environments' `llvm-tools = "==<LLVM version>"`
- `pixi.lock`: `pixi lock`

Every file of the toolchain and of libclang is an input of the actions that read it, so the new release rebuilds everything; nothing has to be cleaned. A release re-published under the same tag needs a new module version on the registry (its archive digests are recorded there).

## Step 4: Regenerate `modules/`

`modules/demand.cpp` records what clice takes from the libraries; `scripts/modules.py` builds it for a configuration, indexes it with clice and wraps the libraries' headers with `clice modularize`, then merges the eight configurations into `modules/`. Push the pinned tree to both boxes, then:

```bash
# Linux box
pixi run python scripts/modules.py --clice ~/clice-bin/bin/clice generate \
    linux-x64 linux-x64-debug linux-arm64 windows-x64 windows-arm64
# macOS box
pixi run python scripts/modules.py --clice ~/clice-bin/bin/clice generate \
    macos-arm64 macos-arm64-debug macos-x64
# here: the macOS wrappings to the Linux box, the merge, the result back
box cp $mac $linux build/modules/macos-arm64 build/modules/macos-arm64-debug build/modules/macos-x64
box run $linux 'pixi run python scripts/modules.py --clice ~/clice-bin/bin/clice merge'
box cp $linux . modules
```

Each configuration takes about half a minute. Nothing under `modules/` but `demand.cpp`, `BUILD.bazel` and `modules.bzl` is edited by hand; a wrong result is fixed in its inputs and regenerated:

- `generate` compiles `demand.cpp` first, so a header that moved or went upstream stops it there: fix the include in `demand.cpp`, as the API adaptation of Step 5 would in clice's own files.
- A warning from `clice modularize` stops it too: a header the partition leaves to the program means a library's include directory moved (the partition's globs are in `scripts/modules.py`).
- What only some configurations export lands under their conditions in `deps.cppm`; what clice uses on one platform alone is written under that platform's `#ifdef` in `demand.cpp` (`llvm/Support/WindowsError.h` under `_WIN32`).
- The macOS wrappings list lmdb, spdlog and simdjson after kotatsu, the Linux ones before it; that order difference alone is expected.

The diff of `modules/` is part of the upgrade's review: names the libraries dropped or renamed, and macros that changed.

## Step 5: Build and Adapt the API

```bash
pixi run build RelWithDebInfo
```

Fix LLVM API breaking changes based on compilation errors. Common categories:

- **Header path changes**: e.g., `clang/Driver/Options.h` → `clang/Options/Options.h`
- **Namespace migrations**: e.g., `clang::driver::options` → `clang::options`
- **Type system changes**: e.g., ElaboratedType removal, NestedNameSpecifier pointer→value
- **Function signature changes**: e.g., `createDiagnostics` parameter changes
- **Type merges/splits**: e.g., DependentTemplateSpecializationType → TemplateSpecializationType

Strategy:

1. Fix header/namespace changes first (mechanical). A header, name or macro clice now needs that `modules/` does not export goes into `demand.cpp`, and Step 4 runs again.
2. Fix type system and signature changes (requires understanding semantics)
3. Update the case outputs upstream changed: `UPDATE_SNAPSHOTS=1`, every changed block reviewed like code (the write-tests skill)
4. Ensure `pixi run -e test-run unit-test RelWithDebInfo` passes
5. Port `clang/lib/AST/StmtProfile.cpp` changes into `src/semantic/expr_hash.cpp` (a trimmed copy of `StmtProfiler` with clice's own leaves): do not diff the files — list the upstream commits with `git log llvmorg-<old>..llvmorg-<new> -- clang/lib/AST/StmtProfile.cpp`, and hand an Opus subagent that list with the instruction to apply each commit's C and C++ visitor changes to the port; `unittests --test-filter=expr_hash` (the bit-for-bit fidelity test against `Stmt::Profile`) must be green afterwards
6. Bump `index_format_version` in `src/index/serialization.cppm`: entity hashes (`src/semantic/identity.cpp`) follow clang's canonicalization rules, so they can change silently across versions and an old index would otherwise keep serving stale symbols
7. A library clice starts using directly is added to `LLVM_LIBRARIES` in `bazel/BUILD.bazel`. xclang's libclang archives carry only the libraries clice uses: one they lack goes into xclang's `toolchain/cmake/caches/clang.cmake` (`LLVM_Development_DISTRIBUTION_COMPONENTS`) first, and needs a new xclang release

When a fix is not obvious, read the LLVM source code to understand the new API. If `../llvm-project` exists locally, use it. Otherwise, look up the upstream commit/PR on GitHub.

Then the unit tests and the cases on the Linux box, and a build of every other configuration on the box that builds it: `pixi run build Debug` on both boxes (the ASan libclang, built with assertions, declares more than the release one), `pixi run build RelWithDebInfo` on macOS, and `pixi run build RelWithDebInfo -- --platforms=@xclang//platforms:<triple>` for the cross ones. CI runs them all, but a configuration that does not build is found faster here.

## Step 6: Write the Changelogs (REQUIRED)

Toolchain-level findings on clice's side (`MODULE.bazel`, `.bazelrc`, `BUILD.bazel`, `bazel/`, CI mechanics; the toolchain itself keeps its own notes in xclang) go to `toolchain-changelog.md` in this directory, in its symptom / cause / fix / check shape. API changes go to `llvm-changelog.md`, maintainer reference material, deliberately not a docs page.

Every LLVM upgrade adds a new H2 section to `llvm-changelog.md` (e.g., `## LLVM 22 → 23`) documenting all breaking changes encountered. For each API change, record:

- Change description
- Upstream commit hash
- PR number (link to `https://github.com/llvm/llvm-project/pull/<NUM>`)
- Impact on clice

To find upstream commits, search the LLVM git history between version tags (`git log --oneline llvmorg-<OLD>..llvmorg-<NEW> -- clang/include/clang/AST/` in `../llvm-project`, or the GitHub history). Group changes by category with a table per category, as the newest section does.

## Step 7: Report and Wait

Present a summary to the maintainer and **wait for confirmation** before opening the PR. The summary should include:

- All API changes that were adapted and how they were resolved
- What the regenerated `modules/` dropped, and what `demand.cpp` gained
- All test expectation changes (snapshot updates, assertion value changes) and why
- Any unavoidable behavior changes from upstream LLVM (e.g., TypePrinter output differences, type sugar changes) that affect user-visible features like hover
- The LLVM changelog that was written

The maintainer decides whether all changes are acceptable or if adjustments are needed.

## Step 8: The PR

The pr skill takes over; the title is `chore: upgrade LLVM to <X.Y.Z>`.

## Notes

- **Other libraries**: a version bump of kotatsu or any other library `modules/` wraps (its `bazel_dep` in `MODULE.bazel`) is Steps 2 and 4 alone, then the build; so is clice starting to use a header or macro `modules/` does not export yet. A new library is added to `LIBRARIES` in `modules/modules.bzl` and to the partition in `scripts/modules.py`.
- **Private headers**: clice includes no private clang headers, and xclang's libclang archives carry none; one clice starts to need goes into xclang's libclang package (`toolchain/` in xclang) first.
- **Debug builds**: ASan builds (`bazel/clice.bazelrc`'s Debug) link the ASan libclang, which xclang builds for x86_64 Linux and arm64 macOS (its `@libclang` follows `--features=asan`); the other targets have no Debug build.
