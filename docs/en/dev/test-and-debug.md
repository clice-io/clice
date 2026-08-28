# Test and Debug

## Run Tests

clice has four types of tests: unit tests, integration tests, smoke tests, and snap tests.

All test dependencies (node/npm for the integration suite and tools, python for scripts/) are managed by pixi — no separate installation needed.

### Unit Tests

```bash
pixi run unit-test          # default RelWithDebInfo
pixi run unit-test Debug    # debug build
```

Equivalent to:

```bash
./build/RelWithDebInfo/bin/unit_tests --verbose
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
npm run check   # typecheck (tsc strict) + lint (ESLint)
CLICE_EXECUTABLE=../build/RelWithDebInfo/bin/clice npm test
```

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
    --clice=./build/RelWithDebInfo/bin/clice
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
CLICE_EXECUTABLE=../build/RelWithDebInfo/bin/clice npm run snap
```

A fixture is a single `.cpp` at the corpus root, or a subdirectory entered through its `main.cpp` — one multi-file unit whose sibling sources (module interfaces, headers, extra sources) belong to the fixture. Corpus-wide compile flags live in the corpus's `corpus.json` manifest; a fixture appends its own with `- flags: [...]`. Each server-path run materializes the fixture into a throwaway workspace (sources arrive on disk with `§`-annotations already stripped), so fixtures never share state and background indexing — off by default, enabled per fixture with `- indexing: true` — sees the same bytes the compiler does. A fixture that deliberately does not compile cleanly declares `- diagnostics: expected`; unexpected diagnostics fail the fixture, and so does a clean compile under that declaration.

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
$ pixi run build                  # build/RelWithDebInfo/bin/clice
$ pixi run -e editor editor-test  # nvim + vscode, both fixtures
```

Prerequisites outside the pixi env:

- `nvim` (stable) on `PATH` for `nvim-e2e`.
- A system `cmake`/`ninja`/`clang` for `editor-prepare` to configure the CMake-based module fixture (same assumption the integration tests make).
- A display (or `xvfb-run`) plus the usual Electron system libraries for `vscode-e2e`.

## Debug

If you want to attach a debugger to clice, start it in socket mode independently, then connect a client.

```shell
./build/Debug/bin/clice serve --mode socket --port 50051
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
