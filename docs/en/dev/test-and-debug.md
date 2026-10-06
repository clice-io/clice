# Test and Debug

## Run Tests

clice has four types of tests that run on every change: unit tests, integration tests, smoke tests, and snap tests. Compatibility tests, which need real build systems and compilers, run separately.

All test dependencies (node/npm for the integration suite and tools, python for scripts/) are managed by pixi — no separate installation needed.

### Unit Tests

```bash
pixi run unit-test          # default RelWithDebInfo
pixi run unit-test Debug    # debug build
```

Equivalent to:

```bash
./build/RelWithDebInfo/bin/bin/unit_tests --verbose
```

### Integration Tests

End-to-end tests that start a real `clice serve` instance and communicate via LSP protocol.

```bash
pixi run integration-test          # default RelWithDebInfo
pixi run integration-test Debug    # debug build
```

The suite is TypeScript on vitest (`tests/`), speaking LSP through the
official vscode-languageserver-protocol stack. Equivalent to:

```bash
cd tests
CLICE_EXECUTABLE=../build/RelWithDebInfo/bin/bin/clice npm test
```

A change to the TypeScript also passes `npm run check` at the repository root: strict tsc and ESLint over every package.

Useful variants:

```bash
npx vitest run --config integration/vitest.config.ts integration/server/memory_ownership.test.ts   # one file
```

### Smoke Tests

Replay recorded LSP sessions to catch regressions in protocol handling.

```bash
pixi run smoke-test          # default RelWithDebInfo
pixi run smoke-test Debug    # debug build
```

Equivalent to:

```bash
node tools/replay.ts tests/smoke/*.jsonl \
    --clice=./build/RelWithDebInfo/bin/bin/clice
```

### Snap Tests

Feature snapshot corpora under `tests/snap/<feature>/`, with sources and snapshots side by side. The snap suite (`tests/snap/snap.test.ts`, domain logic in `tools/snap/`) pins every fixture from the paths its `verify:` mode asks for: inspect (one `clice inspect` process per fixture, no server involved) and server (replayed through a real server). The integration suite plays no part in snapshots.

```bash
pixi run snap-test          # default RelWithDebInfo
pixi run snap-test Debug    # debug build
```

Equivalent to:

```bash
cd tests
CLICE_EXECUTABLE=../build/RelWithDebInfo/bin/bin/clice npm run snap
```

A fixture is a single `.cpp`, or a subdirectory entered through its `main.cpp` — one multi-file unit whose sibling sources (module interfaces, headers, extra sources) belong to the fixture. A fixture that documents a capability lives in a section directory of the corpus as `<section>/NN_name.cpp` (or `<section>/NN_unit/main.cpp`) and opens with a `/// # Capability name` doc header — the name alone, at most five words — followed by its metadata list, where `status` (`supported`, `partial` or `unsupported`) is required, and a one-sentence summary paragraph that becomes the capability card's summary: the directory keys the feature page's generated region, the two-digit number orders the item within it, and the header feeds the page (see `tools/docs/feature.ts`). Edge-case fixtures without a doc header stay at the corpus root. Corpus-wide compile flags live in the corpus's `corpus.json` manifest; a fixture appends its own with `- flags: [...]`. Each server-path run materializes the fixture into a throwaway workspace (sources arrive on disk with `§`-annotations already stripped), so fixtures never share state and background indexing — off by default, enabled per fixture with `- indexing: true` — sees the same bytes the compiler does. A fixture that deliberately does not compile cleanly declares `- diagnostics: expected`; unexpected diagnostics fail the fixture, and so does a clean compile under that declaration.

By default a fixture is `verify: both` with `snap: shared`: the inspect and server results must render byte-identically and are pinned by one `<name>.snap.yml`. A fixture whose two paths legitimately differ declares `- snap: separate` in its `///` doc header (with a `// snap:` comment explaining why) and each path pins its own `<name>.inspect.snap.yml` / `<name>.server.snap.yml`. A known-wrong divergence is declared as `- snap: skip`: the fixture runs nowhere and keeps no snapshot until the two paths agree. A feature that exists on only one path (include and import completion answered by the server; index dumps with no LSP request shape) declares `- verify: server` or `- verify: inspect` and that side owns the plain `<name>.snap.yml`.

`UPDATE_SNAPSHOTS=1` updates everything in one run: inspect tests run first and own shared snapshot bodies; the server side can only update its own variants. A shared snapshot mismatch on the server side is a real divergence between the server pipeline and the direct feature call — investigate it instead of regenerating over it.

### Run All Tests

```bash
pixi run test                # runs unit + integration + smoke + snap
pixi run test Debug          # all tests with debug build
```

## Editor E2E Tests

Smoke tests that run real editors (headless Neovim and VSCode) against a locally built clice binary, covering startup, first diagnostics, hover, definition and completion on two fixtures (including a C++20 modules project). CI runs them in the `test-editor` job on Linux with the latest stable editor releases, on purpose unpinned: the job exists to catch breakage caused by new editor versions.

```bash
$ pixi run build                  # build/RelWithDebInfo/bin/bin/clice
$ pixi run -e editor editor-test  # nvim + vscode, both fixtures
```

Prerequisites outside the pixi env:

- `nvim` (stable) on `PATH` for `nvim-e2e`.
- A system `cmake`/`ninja`/`clang` for `editor-prepare` to configure the CMake-based module fixture (same assumption the integration tests make).
- A display (or `xvfb-run`) plus the usual Electron system libraries for `vscode-e2e`.

## Compatibility Tests

Real build systems and real compilers: each scenario in `tests/compat/scenarios.ts` builds the small project under `tests/compat/project/` with one build system and one toolchain, then runs clice over the compilation database that build wrote. Every translation unit must parse without errors, as it compiled for the real compiler; clice must agree with that compiler on the macros the command's flags imply, which a generated header compares inside clice's own parse; and each file's command must resolve through the compiler and keep or drop the flags the scenario lists. No database is committed: the suite checks what the tools write today.

```bash
pixi run compat-test          # default RelWithDebInfo
```

Each scenario names the compilers of one platform as its CI runner image has them: on Linux the distribution's versioned GCC and Clang plus bear, ccache, meson, ninja, xmake, bazel, zig, Emscripten, the MinGW, RISC-V and Arm cross compilers, and nvcc from the pixi `cuda` environment (`pixi install -e cuda`); on Windows Visual Studio, LLVM and MinGW; on macOS Apple clang and Homebrew's GCC and LLVM. A scenario whose tools are missing is skipped locally and fails in CI. A scenario clice does not support yet names why in `unsupported`: its checks are skipped while its build still runs. CI runs the suite with every build, and weekly against the newest release.

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
