# Test and Debug

## Run Tests

clice has two test suites that run on every change: the unit tests and the cases. Compatibility tests, which need real build systems and compilers, the editor tests and the nightly real projects run separately.

All test dependencies (node/npm for the cases and tools, python for scripts/) are managed by pixi — no separate installation needed.

### Unit Tests

```bash
pixi run unit-test          # default RelWithDebInfo
pixi run unit-test Debug    # debug build
```

Equivalent to:

```bash
./build/RelWithDebInfo/bin/bin/unittests --verbose
```

### Cases

Every other test is a case under `tests/<topic>/<subtopic>/NN_name/`: a small project of its own, the checks to run on it, and `snap.toml`, every output the case pins. A case asks a real `clice serve`, `clice inspect` or a command, through the framework in `tools/testing/`, and the feature pages of these docs show the cases that have a `README.md`. [`tests/README.md`](https://github.com/clice-io/clice/blob/main/tests/README.md) is the contract: how a case is written, how it asserts and how its output reads.

```bash
pixi run cases-test          # default RelWithDebInfo
pixi run cases-test Debug    # debug build
```

Equivalent to:

```bash
CLICE_EXECUTABLE=$PWD/build/RelWithDebInfo/bin/bin/clice npm run cases --workspace tests
```

`CLICE_CASES=hover/` runs the cases under a path. `UPDATE_SNAPSHOTS=1` rewrites their outputs and prints which changed; review the change like code.

A change to the TypeScript also passes `npm run check` at the repository root: strict tsc and ESLint over every package. `npm test --workspace tools` runs the tools' own tests.

### Run All Tests

```bash
pixi run test                # runs unit + cases
pixi run test Debug          # all tests with debug build
```

## Editor E2E Tests

Smoke tests that run real editors (headless Neovim and VSCode) against a locally built clice binary, covering startup, first diagnostics, hover, definition and completion, and on a header the commands that show and switch its compilation context. They run on copies of three cases — a single file, a C++20 modules project and a header with two hosts — that `editor-prepare` writes under `.cache/editors`. CI runs them on the Linux x64 and macOS arm64 legs with the latest stable editor releases, on purpose unpinned: the job exists to catch breakage caused by new editor versions.

```bash
$ pixi run build                  # build/RelWithDebInfo/bin/bin/clice
$ pixi run -e editor editor-test  # nvim + vscode, all three projects
```

Prerequisites outside the pixi env:

- `nvim` (stable) on `PATH` for `nvim-e2e`.
- A display (or `xvfb-run`) plus the usual Electron system libraries for `vscode-e2e`.

## Compatibility Tests

Real build systems and real compilers: each scenario in `tests/compat/scenarios.ts` builds the small project under `tests/compat/project/` with one build system and one toolchain, then runs clice over the compilation database that build wrote. Every translation unit must parse without errors, as it compiled for the real compiler; clice must agree with that compiler on the macros the command's flags imply, which a generated header compares inside clice's own parse; and each file's command must resolve through the compiler and keep or drop the flags the scenario lists. No database is committed: the suite checks what the tools write today.

```bash
pixi run compat-test          # default RelWithDebInfo
```

Each scenario names the compilers of one platform as its CI runner image has them: on Linux the distribution's versioned GCC and Clang plus bear, ccache, meson, ninja, xmake, bazel, zig, Emscripten, the MinGW, RISC-V and Arm cross compilers, and nvcc from the pixi `cuda` environment (`pixi install -e cuda`); on Windows Visual Studio, LLVM and MinGW; on macOS Apple clang and Homebrew's GCC and LLVM. A scenario whose tools are missing is skipped locally and fails in CI. A scenario clice does not support yet names why in `unsupported`: its checks are skipped while its build still runs. CI runs the suite with every build, and weekly against the newest release.

## Real Projects

Every night main's clice runs over the real projects pinned in `tests/nightly/projects.json`: LLVM in four parts, the Linux kernel, abseil, mp-units built as C++20 modules, and clice itself, each part on a CI runner of its own, on Linux, macOS and Windows (the kernel on Linux, clice where Bazel builds it) (`nightly-projects.yml`). A job prepares its project as its build would (configure, generated headers, or the whole kernel build that writes the database), indexes it from a cold cache, checks with the compiler every unit clice reports errors on, then asks `clice inspect` and a server the same requests at the same places of sampled files and compares the answers. A crash, a hang, an index run that does not finish, a failed request, a unit clice fails or reports errors on that the compiler compiles, or a place where the two answers differ fails the job; `tests/README.md` lists every cause. The job summary lists each failure with the command that reproduces it, beside the previous night's time, memory and index size.

```bash
pixi run node tools/nightly/run.ts --jobs                    # the jobs
CLICE_EXECUTABLE=$PWD/build/RelWithDebInfo/bin/bin/clice \
  pixi run node tools/nightly/run.ts abseil --dir ~/nightly  # one job
```

`--steps index` or `--steps sweep` runs one half, `--file` sweeps the named files and `--only` the named requests. Preparing LLVM or the kernel takes a long build and several gigabytes of disk.

## Debug

If you want to attach a debugger to clice, start it in socket mode independently, then connect a client.

```shell
./build/Debug/bin/bin/clice serve --mode socket --port 50051
```

After the server starts, you can connect a client in two ways:

### Connect via VS Code

Configure the clice extension to connect to your running instance:

1. Install the [clice](https://marketplace.visualstudio.com/items?itemName=clice-io.clice) extension.

2. Configure `.vscode/settings.json`:

   ```jsonc
   {
     "clice.executable": "/path/to/your/clice/executable",
     "clice.mode": "socket",
     "clice.port": 50051,
     // Optional: disable clangd if also installed
     "clangd.path": "",
   }
   ```

3. Reload Window (`Developer: Reload Window`) for settings to take effect.

### Debug the VS Code extension

The extension lives in-tree at `editors/vscode/`:

1. Install dependencies:

   ```shell
   npm install # at the repo root; the extension is an npm workspace member
   ```

2. Open the **repository root** in VS Code (the launch configurations are in `.vscode/launch.json` at the root).

3. Create `.vscode/settings.json` with the tcp config above.

4. Press `F5` and select `VSCode Extension (pipe)` or `VSCode Extension (socket)` to launch an Extension Development Host window.
