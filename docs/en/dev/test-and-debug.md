# Test and Debug

## Run Tests

clice has three types of tests: unit tests, integration tests, and smoke tests.

All test dependencies (node/npm for the integration suite and tools, python for scripts/) are managed by pixi — no separate installation needed.

### Unit Tests

```bash
pixi run unit-test          # default RelWithDebInfo
pixi run unit-test Debug    # debug build
```

Equivalent to:

```bash
./build/RelWithDebInfo/bin/unit_tests \
    --test-dir="./tests/data" \
    --snapshot-dir="./tests/snapshots/unit" \
    --verbose
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
npx vitest run integration/features/document_links.test.ts   # one file
UPDATE_SNAPSHOTS=1 npm test    # accept wire-snapshot changes
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

### Run All Tests

```bash
pixi run test                # runs unit + integration + smoke
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
   cd editors/vscode
   pnpm install
   ```

2. Open the **repository root** in VS Code (the launch configurations are in `.vscode/launch.json` at the root).

3. Create `.vscode/settings.json` with the tcp config above.

4. Press `F5` and select `VSCode Extension (pipe)` or `VSCode Extension (socket)` to launch an Extension Development Host window.
