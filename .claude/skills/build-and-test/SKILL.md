---
name: build-and-test
description: Read before building clice or running its test suites — where they run (a box where the `box` command exists, else here), what a change runs, the commands, other platforms, reading the results.
---

# Building and testing clice

## Where it runs

Where the `box` command is installed (`command -v box`; the agent-box skill), every build and suite runs on a box, not on this machine: it hosts every agent's session, and one build saturates it. Only the tools' own tests (`pixi run npm test -w tools`, no build) run here. Without `box`, everything runs here.

Either way, builds and suites go through Bash `run_in_background`, which notifies on exit — not a detached script polled by timed wake-ups.

## What a change runs

- Every push: the unit tests and the cases, RelWithDebInfo, beside CI — push without waiting for them.
- Async code, shutdown or position math: the Debug suites too, beside the RelWithDebInfo ones. ASan and the assertions — clice's, clang's and kotatsu's — live only there. Debug builds Linux x64 and macOS arm64 alone: xclang has the ASan libclang only there.
- Command handling: the compat suite (`pixi run compat-test`). It needs the build systems and compilers CI installs (`compat-test.yml`); where `CI` is set, a box included, a missing one fails its scenario instead of skipping it.
- `editors/`: the editors' end-to-end tests (`pixi run -e editor editor-test`, Zed's `zed-e2e`); on Linux without a display, under `xvfb-run -a`.
- A new command, flag, `clice/*` request or `clice.toml` key: a case that exercises it, or CI's coverage check fails (`tests/README.md`, "Coverage").
- A failure on one platform: reproduced and fixed on that OS — a box of it, or a machine of yours — not by pushing guesses at CI.

## Commands

- Build: `pixi run build [type]` = `npx bazel build --config=[type] //:dist` (npm's bazelisk), the type `RelWithDebInfo` (the default) or `Debug`. Each type has its own output directory: `build/[type]/bin` is Bazel's output tree, with `bin/clice`, `bin/unittests` and the resource directory `lib/clang` in it — the tests and everything else run `build/[type]/bin/bin/clice`.
- More targets and Bazel options go after `--`, built beside `//:dist`: `//:package` and `//:symbols` are the release archive and the symbol package, `clice.gsym` (`build/[type]/bin/clice.tar.gz` and `clice-symbol.tar.xz`, `.zip` on Windows).
- libclang is ThinLTO bitcode: a link redoes its code generation (minutes) unless lld's ThinLTO cache (`bazel/clice.bazelrc`) has it, so the first link on a machine is slow and later ones take seconds.
- `npx bazel` exists once `pixi run install-tests` (or a build) has run. Building on Windows takes a short output root first (`docs/en/guide/build.md`).
- `compile_commands.json` for clice's own sources: `npx bazel run @compdb//:refresh -- --output_groups=+compilation_prerequisites_INTERNAL_` (of `//...`): the added output group builds what the commands read but the aspect does not, the generated headers and the `_virtual_includes` trees, which go stale otherwise.
- Unit tests: `pixi run -e test-run unit-test [type]`; one of them: `-- --test-filter=SuiteName.CaseName` (also `SuiteName.*`).
- Cases: `pixi run -e test-run cases-test [type]`; some of them: `CLICE_CASES=<path prefix> pixi run -e test-run cases-test [type]`, the prefix e.g. `hover/` or `hover/templates/06_template_forms`. The "Running" section of `tests/README.md` lists the rest.
- Both: `pixi run -e test-run test [type]`. The suites run in the `test-run` environment, as in CI.
- Every target also cross-builds on Linux x64, as CI builds them all: `pixi run build RelWithDebInfo -- --platforms=@xclang//platforms:<triple>`, the triples being the `target`s of `.github/workflows/test.yml`. `build/[type]` points at the last build's output tree: after a cross build, the suites here would run the other platform's programs — build natively again first.

## Reading the results

- Write the output to a file (`> /tmp/<topic>-build.log 2>&1`), then read the exit code and the log's tail (`Build did NOT complete successfully`, `FAILED`, the suite summary) — never the whole log, never through a pipe: `| tail` reports tail's exit code (a failed build looks green), `pipefail` + `| grep error` fails when there is nothing to find.
- Before running a suite, check the binary was built after your last source edit (on a box: after your last `box push`): a pass on a stale binary verifies nothing.
- A failing case is named `<topic>/<subtopic>/<case> > <test>`; `CLICE_CASES=<topic>/<subtopic>/<case>` reruns it alone.

## On a box

```bash
id=$(box up clice-io/clice)        # ~1 min
box push $id                       # tracked files only: `git add` new ones first
box run $id 'pixi run build RelWithDebInfo' > /tmp/<topic>-build.log 2>&1
box run $id 'pixi run -e test-run test RelWithDebInfo' > /tmp/<topic>-test.log 2>&1
```

- Boxes are plentiful: verification runs in parallel, never one run after another on one box. Each kind of work gets a box of its own — RelWithDebInfo and Debug, each target, each side of an A/B — and the cases split over several with the shard argument (`pixi run -e test-run cases-test RelWithDebInfo 2/3`, whole subtopics per shard).
- A box is leased once and reused, warm, for every later push; never `box down` it — an idle box stops by itself after an hour.
- Only a linux box restores main's CI caches (Bazel's and ThinLTO's), of Linux x64 and Windows x64 RelWithDebInfo: a Debug build or another cross target starts cold.
- Another platform: build on a linux box with the platform's triple, run on a box of that platform (`--os`). That box only runs the suites (a windows box builds nothing: no Bazel cache, only the `test-run` environment):

```bash
lin=$(box up clice-io/clice)
win=$(box up clice-io/clice --os windows)    # or windows-arm, macos, macos-intel, linux-arm
box push $lin && box push $win
box run $lin 'pixi run build RelWithDebInfo -- --platforms=@xclang//platforms:x86_64-w64-mingw32'
box cp $lin $win build/RelWithDebInfo/bin/bin build/RelWithDebInfo/bin/lib --exclude '*.runfiles' --exclude '*.dSYM'
box run $win 'pixi run -e test-run unit-test RelWithDebInfo'
```
