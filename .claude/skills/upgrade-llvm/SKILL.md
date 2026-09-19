---
name: upgrade-llvm
description: Complete workflow for upgrading the prebuilt LLVM packages clice depends on — build-llvm CI, API adaptation, release-llvm publishing, changelog. Arg = target version, e.g. 22.1.4.
---

Upgrade LLVM to a new version. Accepts the target version as argument (e.g., `22.1.4`).

This is the complete workflow for upgrading the LLVM prebuilt packages that clice depends on. Follow each step in order. Steps that involve CI should use polling (check every ~5 minutes) to wait for completion.

## Step 1: Validate the Package Definition Locally, Then Trigger the Build

The package is built from an explicit component list (`COMPONENTS` in `scripts/build-llvm.py`), and that list drifts between LLVM versions: libraries appear, split or disappear. Validate it against the new version before spending CI time:

```bash
cd ../llvm-project && git checkout llvmorg-<VERSION>   # or a worktree at that tag
pixi run -e package python3 scripts/build-llvm.py --llvm-src ../llvm-project \
  --runtimes-src <checkout at the pixi clang's release> \
  --mode RelWithDebInfo --build-dir ../llvm-project/build-validate --configure-only
```

This builds libc++ (minutes) and configures LLVM without building it. The configure fails on both kinds of drift: an entry whose library no longer exists ("doesn't have an install target") and a library the closure now needs but the list lacks ("requires target X that is not in any export set"). Fix `COMPONENTS` until it passes; a Debug run (`--mode Debug`) covers the ASan variant. Never do this by pushing attempts at CI.

The libc++ in the package follows the compiler, not the LLVM being packaged: `--runtimes-src` is the llvm-project checkout of the pixi clang's release (the workflow derives it from `clang --version`; omit it locally when the two versions coincide). Patches in clice-llvm live under `patches/<version>/` and apply to the checkout of that version, so a libc++ patch belongs to the compiler's version directory. Windows and macOS runtimes quirks and their patches are recorded in the clice-llvm README.

Configure-level validation cannot see link-time problems (the Windows CRT's `detect_mismatch` directives, symbols libc++ takes from the MSVC STL); the first CI round is the real test for those, and a failure there is reproduced locally with a small program linked against the freshly built `libc++.lib`, not by rebuilding LLVM.

Trigger the `build-llvm` workflow on GitHub Actions:

```bash
gh workflow run build-llvm.yml \
  --ref <BRANCH> \
  --field llvm_version="<VERSION>"
```

`--ref` makes the run use the branch's `scripts/build-llvm.py` and workflow; without it the dispatch runs `main`'s.

- Poll until all 14 matrix builds complete, note the workflow run ID

## Step 2: Download Local Platform Artifact

Download the artifact matching the development machine:

```bash
gh run view <RUN_ID>
gh run download <RUN_ID> -n x86_64-unknown-linux-gnu.releasedbg.tar.xz -D .llvm-download
mkdir -p .llvm
tar -xf .llvm-download/x86_64-unknown-linux-gnu.releasedbg.tar.xz -C .llvm
```

Configure clice to build against it:

```bash
pixi run cmake-config RelWithDebInfo ON -- "-DLLVM_INSTALL_PATH=.llvm"
pixi run cmake-build RelWithDebInfo
```

An existing build directory caches `LLVM_DIR` and `Clang_DIR` from the previous package, and `find_package` honours them before the `PATHS` we pass — the new package is silently ignored. Add `-ULLVM_DIR -UClang_DIR` to the `--` arguments, or use a fresh build directory.

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
7. A library clice starts using directly is added to `cmake/llvm.cmake` and to `COMPONENTS` in `scripts/build-llvm.py` (the package ships exactly that closure); re-run the Step 1 validation

When a fix is not obvious, read the LLVM source code to understand the new API. If `../llvm-project` exists locally, use it. Otherwise, look up the upstream commit/PR on GitHub.

## Step 4: Create PR

```bash
git checkout -b chore/upgrade-llvm-XX
git add -A
git commit -m "chore: upgrade LLVM to XX.Y.Z"
git push -u origin chore/upgrade-llvm-XX
gh pr create --title "chore: upgrade LLVM to XX.Y.Z" --body "..."
```

CI will fail at this point (manifest hashes are stale) — this is expected.

## Step 5: Run Release LLVM Workflow

Trigger `release-llvm` to publish the artifacts of the Step 1 run:

```bash
gh workflow run release-llvm.yml \
  --ref <BRANCH> \
  --field source_run_id="<STEP1_RUN_ID>" \
  --field llvm_version="<VERSION>"
```

This creates (or reuses) the clice-llvm release and uploads the 14 archives as built. Poll until complete.

The package contains only the libraries clice links: `COMPONENTS` is the transitive closure of the libraries `cmake/llvm.cmake` names (validated in Step 1).

## Step 6: Update Version

Update the version string in `cmake/package.cmake`:

```
setup_llvm("<VERSION>")
```

Commit and push:

```bash
git add cmake/package.cmake
git commit -m "chore: update LLVM to <VERSION>"
git push
```

Poll CI until all platforms pass. CMake downloads the correct artifact automatically based on the version and platform — no manifest file needed. Local build directories keep building against the old package: `setup_llvm` skips the download while the cached `LLVM_INSTALL_PATH` still points at an existing install, and `find_package` keeps the cached `LLVM_DIR`/`Clang_DIR`. Reconfigure with `-ULLVM_INSTALL_PATH -ULLVM_DIR -UClang_DIR`, or use a fresh build directory.

## Step 7: Write LLVM Changelog (REQUIRED)

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

## Step 8: Report to User

Present a summary to the user and **wait for confirmation** before considering the upgrade complete. The summary should include:

- All API changes that were adapted and how they were resolved
- All test expectation changes (snapshot updates, assertion value changes) and why
- Any unavoidable behavior changes from upstream LLVM (e.g., TypePrinter output differences, type sugar changes) that affect user-visible features like hover
- The LLVM changelog that was written

The user decides whether all changes are acceptable or if adjustments are needed. Do NOT push final changes or mark the work as done until the user confirms.

## Notes

- **Artifact size limit**: GitHub Release max 2GB per file. macOS LTO artifacts are largest, currently ~1.7GB with xz -9e.
- **Package contents**: `LLVM_TARGETS_TO_BUILD` is empty (clice generates no code) and only `COMPONENTS` are built, so a configure that passes locally with `--configure-only` is what CI builds.
- **Private headers**: clice depends on private Clang Sema headers (TreeTransform.h etc.), copied from source during `build-llvm.py`. Users must use our packaged LLVM.
