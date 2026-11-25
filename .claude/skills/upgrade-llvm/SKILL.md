---
name: upgrade-llvm
description: Read before upgrading xclang/LLVM (arg = the xclang version, e.g. 23.1.2.1), touching MODULE.bazel, .bazelrc, BUILD.bazel or bazel/, or regenerating modules/ — after a library bump, or when clice needs a header or macro it does not export.
---

Upgrade LLVM to a new version. Accepts the xclang version as argument (e.g., `23.1.2.1`: LLVM 23.1.2, xclang's first build of it).

The compiler clice builds with and the LLVM/Clang libraries it links both come from one [xclang](https://github.com/clice-io/xclang) release: xclang's Bazel module (registry https://bazel.clice.io) brings the toolchain and the release's `libclang-<version>-<triple>[-asan].tar.xz` archives as repositories, and the conda package `xclang` (channel `https://conda.clice.io`) the LLVM tools the tests and the packaging run. They always move together: the archives hold ThinLTO bitcode, which only the LLVM that wrote it is sure to read. clice imports libclang's headers, with every other third-party library's, through the generated module in `modules/`, so an upgrade regenerates it before anything builds. Follow each step in order.

**Read `toolchain-changelog.md` in this directory before touching `MODULE.bazel`, `.bazelrc`, `BUILD.bazel` or `bazel/`**: it records the platform pitfalls that still apply (symptom, cause, fix, how to check). Every new one is added there in the same shape; a toolchain change without an entry is not finished.

The work happens on a branch `chore/upgrade-llvm-<major>`, created from fresh `origin/main` (CLAUDE.md) before Step 2, and runs on Linux boxes (the agent-box skill): one, leased once and kept for the whole upgrade, regenerates `modules/` for every configuration; the builds and suites of Step 5 get a box each.

## Step 1: The xclang Release

The toolchain and the libraries are built in the xclang repository, by its own pipeline; a new LLVM version there is a change of its `LLVM_VERSION` and sources, and ends with a release tagged `<llvm version>.<revision>`, the conda packages on conda.clice.io and the Bazel module on bazel.clice.io. Upgrading clice starts once that release exists:

```bash
gh release view <VERSION> -R clice-io/xclang --json assets --jq '.assets[].name' | grep libclang
```

Every target clice builds needs its `libclang-<VERSION>-<triple>.tar.xz`, and the Debug builds (x86_64 Linux, arm64 macOS) their `-asan` archives.

## Step 2: A clice to Regenerate With

On the Linux box, build clice from this tree before any pin moves, and keep it aside:

```bash
pixi run build RelWithDebInfo
rm -rf ~/clice-bin && cp -RL build/RelWithDebInfo/bin ~/clice-bin
cp "$(npx bazel info output_base)"/external/xclang++xclang+libclang_x86_64-unknown-linux-gnu/include/clang/Options/Options.inc ~/Options.inc.old
```

The last line keeps the old release's table of clang's options for Step 6; a box that skips the build takes it from the old release's `libclang-<version>-x86_64-unknown-linux-gnu.tar.xz`.

The new headers need not build the old `modules/` (a name it exports may be gone upstream), and regenerating only reads them. But the clice must carry this tree's own `clice modularize`: one built from an older tree writes another shape (thousands of extra lines of enum wrappers). main's CI build artifact (`build-<triple>-RelWithDebInfo`, kept 3 days) serves as well: unpacked into `~/clice-bin`, it needs `chmod +x ~/clice-bin/bin/*` (artifacts drop the executable bit), and a box that skips the build needs `pixi run install-tests` before Step 4, or `npx bazel` gets a 404.

## Step 3: Pin It

Move every pin in one change:

- `MODULE.bazel`: `bazel_dep(name = "xclang", version = "<VERSION>")`
- `pixi.toml`: every `xclang = "==<VERSION>"`, and the Windows test environments' `llvm-tools = "==<LLVM version>"`
- `pixi.lock`: `pixi lock`

Every file of the toolchain and of libclang is an input of the actions that read it, so the new release rebuilds everything; nothing has to be cleaned. A release re-published under the same tag needs a new module version on the registry (its archive digests are recorded there).

## Step 4: Regenerate `modules/`

`modules/demand.cpp` records what clice takes from the libraries; `tools/modules/modules.ts` builds it for a configuration, indexes it with clice and wraps the libraries' headers with `clice modularize`, then merges the eight configurations into `modules/`. Push the pinned tree to the Linux box, then:

```bash
box run $linux 'pixi run node tools/modules/modules.ts --clice ~/clice-bin/bin/clice generate'
box run $linux 'pixi run node tools/modules/modules.ts --clice ~/clice-bin/bin/clice merge'
box cp $linux . modules
```

Each configuration takes about half a minute. The macOS ones compile against Apple's SDK, which the first of them fetches (`MODULE.bazel` accepts its license). Nothing under `modules/` but `demand.cpp`, `BUILD.bazel` and `modules.bzl` is edited by hand; a wrong result is fixed in its inputs and regenerated:

- `generate` compiles `demand.cpp` first, so a header that moved or went upstream stops it there: fix the include in `demand.cpp`, as the API adaptation of Step 5 would in clice's own files.
- A warning from `clice modularize` stops it too: a header the partition leaves to the program means a library's include directory moved (the partition's globs are in `tools/modules/modules.ts`).
- What only some configurations export lands under their conditions in `third_party.cppm` and `libc.cppm`; what clice uses on one platform alone is written under that platform's `#ifdef` in `demand.cpp` (`llvm/Support/WindowsError.h` under `_WIN32`).

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
5. Port `clang/lib/AST/StmtProfile.cpp` changes into `src/semantic/expr_hash.cpp` (a trimmed copy of `StmtProfiler` with clice's own leaves): do not diff the files — list the upstream commits with `git log llvmorg-<old>..llvmorg-<new> -- clang/lib/AST/StmtProfile.cpp`, and hand an Opus subagent that list with the instruction to apply each commit's C and C++ visitor changes to the port; `pixi run -e test-run unit-test RelWithDebInfo -- --test-filter='expr_hash.*'` (the bit-for-bit fidelity test against `Stmt::Profile`) must be green afterwards
6. Bump `index_format_version` in `src/index/format.cppm`: entity hashes (`src/semantic/identity.cpp`) follow clang's canonicalization rules, so they can change silently across versions and an old index would otherwise keep serving stale symbols
7. A library clice starts using directly is added to `LLVM_LIBRARIES` in `bazel/BUILD.bazel`. xclang's libclang archives carry only the libraries clice uses: one they lack goes into xclang's `toolchain/cmake/caches/clang.cmake` (`LLVM_Development_DISTRIBUTION_COMPONENTS`) first, and needs a new xclang release

When a fix is not obvious, read the LLVM source code to understand the new API. If `../llvm-project` exists locally, use it. Otherwise, look up the upstream commit/PR on GitHub.

Then the unit tests and the cases, and a build of every other configuration, each on a box of its own (the build-and-test skill): `pixi run build Debug` and `pixi run build Debug -- --platforms=@xclang//platforms:aarch64-apple-darwin` (the ASan libclang, built with assertions, declares more than the release one), and `pixi run build RelWithDebInfo -- --platforms=@xclang//platforms:<triple>` for the other targets. CI runs them all, but a configuration that does not build is found faster here.

## Step 6: Re-check the Option Classes

`src/command/option_classes.inc` classes clang's options by what clice's analysis reads (its header states the criterion), over two rules from clang's tables: an option a release adds or changes takes the class those rules give it until a line says otherwise. After the build, on the Linux box of Step 2, `tools/options/options.ts` gathers the evidence for each new or changed option and its counterpart: the class the table gives it, the macros, `__has_feature` answers and cc1 arguments it changes with a real clang of the release, and the files of the driver reading it for a decision of its own:

```bash
box run $linux 'git clone -q --depth 1 --filter=blob:none --sparse -b llvmorg-<LLVM version> https://github.com/llvm/llvm-project ~/llvm-project-<LLVM version> && git -C ~/llvm-project-<LLVM version> sparse-checkout set clang/lib/Driver'
box run $linux 'pixi run node tools/options/options.ts --previous ~/Options.inc.old --llvm ~/llvm-project-<LLVM version> --out build/options.md'
box cp $linux . build/options.md
```

Hand the report and `option_classes.inc` to codex (the external-agents skill), to decide each suspicious and each new option against the criterion of the `.inc`'s header and answer each class it disputes with a counterexample: the command, the target, and what the analysis then reads otherwise. Probe each one with the release's clang, apply those that hold to the `.inc` (a line in its section, under a comment saying what the analysis reads), run the tool again, and resume the same codex session with the new report until a round brings no new confirmed counterexample.

Then check the whole table against what reads it, each report listing only what is new; a finding is an option to class again, or a disagreement clice keeps by design, added with its reason to the accepted list of the tool that reported it:

- `tools/options/agreement.ts`: clice against clang. Every option the table keeps, with its sample values on its targets, becomes a compile_commands.json clice compiles (`clice inspect`), checked against clang's own macros and `__has_feature` answers for the command. A crash or a disagreement is a finding. About 40 minutes on the Linux box, after the build:

  ```bash
  box run $linux 'pixi run node tools/options/agreement.ts --clice build/RelWithDebInfo/bin/bin/clice --out build/agreement.md'
  ```

- `tools/options/compilers.ts`: the options the table leaves out, against the compilers clice queries but does not compile with: GCC's macros and include search for a GCC command, cl's macros for a cl command. A left-out option that changes them is a finding. GCC on a Linux box with the cross compilers compat installs and a few more, cl on a Windows box (its `/PD`), with the release's option table copied over:

  ```bash
  box run $linux 'sudo apt-get install -y -qq g++ g++-aarch64-linux-gnu g++-arm-linux-gnueabihf g++-riscv64-linux-gnu g++-powerpc64le-linux-gnu g++-s390x-linux-gnu g++-mipsel-linux-gnu g++-mingw-w64-x86-64 g++-mingw-w64-i686 gcc-arm-none-eabi gcc-riscv64-unknown-elf'
  box run $linux 'pixi run node tools/options/compilers.ts --out build/gcc.md $(for g in gcc g++ aarch64-linux-gnu-g++ arm-linux-gnueabihf-g++ riscv64-linux-gnu-g++ powerpc64le-linux-gnu-g++ s390x-linux-gnu-g++ mipsel-linux-gnu-g++ x86_64-w64-mingw32-g++ i686-w64-mingw32-g++ arm-none-eabi-gcc riscv64-unknown-elf-gcc; do echo --gcc=$(command -v $g); done)'
  box run $linux 'mkdir -p build/libclang/clang && cp -rL $(npx bazel info output_base)/external/xclang++xclang+libclang_x86_64-unknown-linux-gnu/include/clang/Options build/libclang/clang/'
  box cp $linux $windows build/libclang
  box run $windows 'vs=$("/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" -latest -property installationPath); pixi run node tools/options/compilers.ts --include build/libclang --cl "$(ls "$vs"/VC/Tools/MSVC/*/bin/Hostx64/x64/cl.exe | tail -n 1)" --out build/cl.md'
  ```

The unit tests and the cases run after the last change.

## Step 7: Write the Changelogs (REQUIRED)

Toolchain-level findings on clice's side (`MODULE.bazel`, `.bazelrc`, `BUILD.bazel`, `bazel/`, CI mechanics; the toolchain itself keeps its own notes in xclang) go to `toolchain-changelog.md` in this directory, in its symptom / cause / fix / check shape. API changes go to `llvm-changelog.md`, maintainer reference material, deliberately not a docs page.

Every LLVM upgrade adds a new H2 section to `llvm-changelog.md` (e.g., `## LLVM 22 → 23`) documenting all breaking changes encountered. For each API change, record:

- Change description
- Upstream commit hash
- PR number (link to `https://github.com/llvm/llvm-project/pull/<NUM>`)
- Impact on clice

To find upstream commits, search the LLVM git history between version tags (`git log --oneline llvmorg-<OLD>..llvmorg-<NEW> -- clang/include/clang/AST/` in `../llvm-project`, or the GitHub history). Group changes by category with a table per category, as the newest section does.

## Step 8: Report and Wait

Present a summary to the maintainer and **wait for confirmation** before opening the PR. The summary should include:

- All API changes that were adapted and how they were resolved
- What the regenerated `modules/` dropped, and what `demand.cpp` gained
- All test expectation changes (snapshot updates, assertion value changes) and why
- The option classes Step 6 changed, and what each counterexample showed
- Any unavoidable behavior changes from upstream LLVM (e.g., TypePrinter output differences, type sugar changes) that affect user-visible features like hover
- The LLVM changelog that was written

The maintainer decides whether all changes are acceptable or if adjustments are needed.

## Step 9: The PR

The pull-request skill takes over; the title is `chore: upgrade LLVM to <X.Y.Z>`.

## Notes

- **Other libraries**: a version bump of kotatsu or any other library `modules/` wraps (its `bazel_dep` in `MODULE.bazel`) is Steps 2 and 4 alone, then the build; so is clice starting to use a header or macro `modules/` does not export yet. A new library is added to `LIBRARIES` in `modules/modules.bzl` and to the partition in `tools/modules/modules.ts`.
- **Private headers**: clice includes no private clang headers, and xclang's libclang archives carry none; one clice starts to need goes into xclang's libclang package (`toolchain/` in xclang) first.
- **Debug builds**: ASan builds (`bazel/clice.bazelrc`'s Debug) link the ASan libclang, which xclang builds for x86_64 Linux and arm64 macOS (its `@libclang` follows `--features=asan`); the other targets have no Debug build.
