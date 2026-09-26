---
name: upgrade-llvm
description: Complete workflow for upgrading the LLVM clice builds with and links — the xclang release, the pins, API adaptation, changelog. Arg = the xclang version, e.g. 23.1.2.1.
---

Upgrade LLVM to a new version. Accepts the xclang version as argument (e.g., `23.1.2.1`: LLVM 23.1.2, xclang's first build of it).

The compiler clice builds with and the LLVM/Clang libraries it links both come from one [xclang](https://github.com/clice-io/xclang) release: the conda package `xclang` (channel `https://conda.clice.io`) and the release's `libclang-<version>-<triple>[-asan].tar.xz` archives, which `cmake/llvm.cmake` downloads. They always move together: the archives hold ThinLTO bitcode, and `cmake/llvm.cmake` refuses a compiler of another LLVM version. Follow each step in order. Steps that involve CI should use polling (check every ~5 minutes) to wait for completion.

**Read `toolchain-changelog.md` in this directory before touching `cmake/llvm.cmake` or `cmake/toolchain.cmake`**: it records every platform pitfall met so far (symptom, cause, fix, how to check). Every new one is appended there in the same shape; a toolchain change without an entry is not finished.

## Step 1: The xclang Release

The toolchain and the libraries are built in the xclang repository, by its own pipeline; a new LLVM version there is a change of its `LLVM_VERSION` and sources, and ends with a release tagged `<llvm version>.<revision>` and the conda packages on conda.clice.io. Upgrading clice starts once that release exists:

```bash
gh release view <VERSION> -R clice-io/xclang --json assets --jq '.assets[].name' | grep libclang
```

Every target clice builds needs its `libclang-<VERSION>-<triple>.tar.xz`, and the Debug legs (x86_64 Linux, arm64 macOS) their `-asan` archives.

## Step 2: Pin It and Build

Move every pin in one change:

- `pixi.toml`: every `xclang = "==<VERSION>"`
- `cmake/package.cmake`: `setup_llvm("<VERSION>")`
- `pixi.lock`: `pixi lock`

Build in a fresh build directory:

```bash
pixi run cmake-config RelWithDebInfo ON
pixi run cmake-build RelWithDebInfo
```

An existing build directory keeps the compiler it detected, and the manifest check then compares the new archive with the old compiler; it also keeps `LLVM_INSTALL_PATH`, `LLVM_DIR` and `Clang_DIR` of the old archive. A release re-published under the same tag is invisible to CPM's cache: delete `~/.cache/clice/cpm/llvm_prebuilt/` first.

Compilation will likely fail — that's what Step 3 addresses.

## Step 3: Adapt API Changes

Fix LLVM API breaking changes based on compilation errors. Common categories:

- **Header path changes**: e.g., `clang/Driver/Options.h` → `clang/Options/Options.h`
- **Namespace migrations**: e.g., `clang::driver::options` → `clang::options`
- **Type system changes**: e.g., ElaboratedType removal, NestedNameSpecifier pointer→value
- **Function signature changes**: e.g., `createDiagnostics` parameter changes
- **Type merges/splits**: e.g., DependentTemplateSpecializationType → TemplateSpecializationType

Strategy:

1. Fix header/namespace changes first (mechanical)
2. Fix type system and signature changes (requires understanding semantics)
3. Update test expectations (AST structure changes affect test output)
4. Ensure `pixi run unit-test RelWithDebInfo` passes
5. Port `clang/lib/AST/StmtProfile.cpp` changes into `src/semantic/expr_hash.cpp` (a trimmed copy of `StmtProfiler` with clice's own leaves): do not diff the files — list the upstream commits with `git log llvmorg-<old>..llvmorg-<new> -- clang/lib/AST/StmtProfile.cpp`, and hand an agent that list with the instruction to apply each commit's C and C++ visitor changes to the port; `unit_tests --test-filter=expr_hash` (the bit-for-bit fidelity test against `Stmt::Profile`) must be green afterwards
6. Bump `index_format_version` in `src/index/serialization.h`: entity hashes (`src/semantic/identity.cpp`) follow clang's canonicalization rules, so they can change silently across versions and an old index would otherwise keep serving stale symbols
7. A library clice starts using directly is added to `cmake/llvm.cmake`; xclang's libclang archives carry every LLVM and clang library, so nothing else changes

When a fix is not obvious, read the LLVM source code to understand the new API. If `../llvm-project` exists locally, use it. Otherwise, look up the upstream commit/PR on GitHub.

## Step 4: Create PR

```bash
git checkout -b chore/upgrade-llvm-XX
git add -A
git commit -m "chore: upgrade LLVM to XX.Y.Z"
git push -u origin chore/upgrade-llvm-XX
gh pr create --title "chore: upgrade LLVM to XX.Y.Z" --body "..."
```

## Step 5: Write the Changelogs (REQUIRED)

Toolchain-level findings on clice's side (`cmake/llvm.cmake`, `cmake/toolchain.cmake`, CI mechanics; the toolchain itself keeps its own notes in xclang) go to `toolchain-changelog.md` in this directory, in its symptom / cause / fix / check table shape. API changes go to `llvm-changelog.md` as described below.

**Every LLVM upgrade MUST append to `llvm-changelog.md` in this skill's directory** (`.claude/skills/upgrade-llvm/llvm-changelog.md`). It is maintainer reference material, deliberately not a docs page.

Add a new H2 section (e.g., `## LLVM 22 → 23`) documenting all breaking changes encountered. For each API change, record:

- Change description
- Upstream commit hash
- PR number (link to `https://github.com/llvm/llvm-project/pull/<NUM>`)
- Impact on clice

To find upstream commits, search the LLVM git history between version tags:

```bash
# If ../llvm-project exists locally:
cd ../llvm-project
git log --oneline llvmorg-<OLD>..llvmorg-<NEW> -- clang/include/clang/AST/
```

If the LLVM source is not available locally, look up changes on GitHub by searching the LLVM repository commit history.

Group changes by category (Type System, NNS, Driver/Frontend, Other) with a table per category. See the existing `LLVM 21 → 22` section as a template.

## Step 6: Report to User

Present a summary to the user and **wait for confirmation** before considering the upgrade complete. The summary should include:

- All API changes that were adapted and how they were resolved
- All test expectation changes (snapshot updates, assertion value changes) and why
- Any unavoidable behavior changes from upstream LLVM (e.g., TypePrinter output differences, type sugar changes) that affect user-visible features like hover
- The LLVM changelog that was written

The user decides whether all changes are acceptable or if adjustments are needed. Do NOT push final changes or mark the work as done until the user confirms.

## Notes

- **Private headers**: clice includes private clang Sema headers (`TreeTransform.h`, `TypeLocBuilder.h`, `CoroutineStmtBuilder.h`), which xclang copies into its libclang archives from the source; one more goes into xclang's `scripts/toolchain.ts` first. Users must use xclang's libclang.
- **Debug builds**: ASan exists for the targets xclang builds an ASan libclang for (`clice_asan_available` in `cmake/llvm.cmake`); Debug builds for the other targets link the release archive without ASan.
