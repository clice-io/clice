Upgrade LLVM to a new version. Accepts the target version as argument (e.g., `22.1.4`).

This is the complete workflow for upgrading the LLVM prebuilt packages that clice depends on. Follow each step in order. Steps that involve CI should use polling (check every ~5 minutes) to wait for completion.

## Step 1: Trigger LLVM Build

Trigger the `build-llvm` workflow on GitHub Actions:

```bash
gh workflow run build-llvm.yml \
  --field llvm_version="<VERSION>" \
  --field skip_clice_build=true
```

- `skip_clice_build=true`: new LLVM APIs likely have breaking changes, skip clice compilation
- Poll until all 14 matrix builds complete (~2-3 hours), note the workflow run ID

## Step 2: Download Local Platform Artifact

Download the artifact matching the development machine:

```bash
gh run view <RUN_ID>
gh run download <RUN_ID> -n x64-linux-gnu-releasedbg.tar.xz -D .llvm-download
mkdir -p .llvm
tar -xf .llvm-download/x64-linux-gnu-releasedbg.tar.xz -C .llvm
```

Configure clice to build against it:

```bash
pixi run cmake-config RelWithDebInfo ON -- "-DLLVM_INSTALL_PATH=.llvm/build-install"
pixi run cmake-build RelWithDebInfo
```

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

Trigger `release-llvm` to build pruned packages:

```bash
gh workflow run release-llvm.yml \
  --field source_run_id="<STEP1_RUN_ID>" \
  --field llvm_version="<VERSION>"
```

This will: discover unused libs → create clice-llvm release → repackage with pruning → upload manifest. Poll until complete.

## Step 6: Update Manifest

```bash
gh run list --workflow release-llvm.yml --limit 1
gh run download <RELEASE_RUN_ID> -n llvm-manifest-final -D .
python3 scripts/update-llvm-version.py \
  --version "<VERSION>" \
  --manifest-src llvm-manifest.json \
  --manifest-dest config/llvm-manifest.json \
  --package-cmake cmake/package.cmake
git add config/llvm-manifest.json cmake/package.cmake
git commit -m "chore: update llvm-manifest.json to <VERSION>"
git push
```

Poll CI until all platforms pass.

## Step 7: Write LLVM Changelog (REQUIRED)

**Every LLVM upgrade MUST append to `docs/en/changelog/llvm-changelog.md`.**

Add a new H1 section (e.g., `# LLVM 22 → 23`) documenting all breaking changes encountered. For each API change, record:

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
- **Pruning safety**: discover phase validates by deleting .a files one by one and rebuilding clice. clang-tidy modules can't be deleted due to force-link.
- **Version cache**: cmake download logic writes `.llvm/.llvm-version` stamp, auto-cleans on version change.
- **Private headers**: clice depends on private Clang Sema headers (TreeTransform.h etc.), copied from source during `build-llvm.py`. Users must use our packaged LLVM.
