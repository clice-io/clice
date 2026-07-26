---
name: write-tests
description: How to write clice integration tests (TypeScript/vitest) — fixture forms, Workspace/CliceClient API, snapshot workflow, hard rules and known pitfalls. Read BEFORE writing or modifying anything under tests/.
---

# Writing clice integration tests

The suite is TypeScript on vitest. Harness = the `@clice/tools` workspace
package (`tools/`); vitest glue = `tests/fixtures.ts`. Tests live in
`tests/integration/<area>/*.test.ts`; tests of the tooling itself in
`tests/tools/`. Run: `cd tests && CLICE_EXECUTABLE=../build/RelWithDebInfo/bin/clice npx vitest run <file>`;
gates: `npm run check` at the repo root (tsc strict + ESLint, zero tolerance).

## Choosing a fixture form

1. **All tests target one data workspace** (`tests/data/<name>`): the bound
   form — zero boilerplate, teardown fully automatic.

   ```ts
   import { cliceTest, expect } from "../../fixtures.ts";
   const test = cliceTest("document_links");

   test("links with pch", async ({ client, workspace }) => {
       const [uri] = await client.openAndWait("main.cpp"); // workspace-relative
       ...
   });
   ```

2. **Anything else** (several servers, temp workspaces, custom argv,
   per-test options): the `session` factory — the test's resource manager.
   Everything it vends is reclaimed in teardown (shutdown gate, anomaly
   gate, directory removal); never write try/finally cleanup.

   ```ts
   import { expect, test } from "../../fixtures.ts";

   test("rebuild after restart", async ({ session }) => {
       const ws = session.tmpdir();              // auto-removed Workspace
       ws.write("main.cpp", "int main() {}\n");  // relative path, auto-mkdir
       ws.writeCDB(["main.cpp"]);

       const first = await session.spawn(ws).initialize(ws);
       await first.openAndWait("main.cpp");
       await first.shutdown();                   // explicit mid-test shutdown is fine

       const second = await session.spawn(ws).initialize(ws);
       ...                                       // teardown owns `second`
   });
   ```

   Variants: `session("name", opts)` (data workspace, locked + initialized),
   `session.tmp()` (tmpdir + un-initialized server), `session.bare()` (no
   workspace). Options: `initializationOptions`, `allowAnomaly` (ONLY for
   tests that deliberately crash workers — assert on the anomaly
   explicitly), `drainStderr: false` (backpressure tests), `args`,
   `socketPort`.

3. **Wire snapshot tests** (whole-document feature output): don't write
   assertions at all — add a fixture to the corpus `tests/data/<feature>/`
   and the driver `tests/integration/features/snapshots.test.ts` pins the
   reply under `tests/snapshots/integration/`. Position-dependent fixtures
   carry `§(name)` annotations (see `@clice/tools/annotation`). Accept
   intentional changes with `UPDATE_SNAPSHOTS=1 npx vitest run ...` and
   review the diff like code.

## API cheat sheet

`Workspace` (`@clice/tools/workspace`): `path(rel)` `uri(rel)` `write`
`read` `exists` `mkdir` `rm` `writeCDB(files, {extraArgs, std})`
`writeEntries` `generateCDB()` `pinCacheDir()` and cache inspection
(`pchFiles()` `pcmFiles()` `tmpFiles()` `readCacheJson()`). Raw string
path: `ws.root`. Exotic fs ops: `node:fs` + `ws.path(...)`.

`CliceClient` (`@clice/tools/client`): after `initialize(ws)` all paths may
be workspace-relative. Requests: `hoverAt` `definitionAt` `referencesAt`
`completionAt` `documentLinks` `foldingRanges` `semanticTokensFull`
`inlayHints` `formatDocument` ... Documents: `open` `openAndWait` `change`
`save` `close`. Waiting: `armDiagnostics` (arm BEFORE the trigger) /
`waitDiagnostics` / `waitForRecompile` / `waitForIndex` /
`waitForReference`. Asserts: `assertNoErrors` `assertHasErrors`
`assertDiagnosticsCount` `assertCleanCompile` `assertNoAnomaly` `errors`.
Lifecycle: `shutdown()` `killServer()` `assertExitedCleanly()`. Custom
protocol (typed): `queryContext` `currentContext` `switchContext` `poll`
`stats` `logFlood`; raw wire: `sendRequest(TypeOrMethod, params, token?)`,
`onNotification`. Custom protocol types live in `@clice/tools/protocol` —
NEVER redeclare them locally (the VSCode extension shares them).

Timing: use `sleep`, `MTIME_GRANULARITY`, `SETTLE_TIME`, `IDLE_TIMEOUT`
from `@clice/tools/client` — never bare magic-number sleeps, and prefer
deterministic waits (`poll("cdb")`, `armDiagnostics`) over sleeping.

## Hard rules

- **Never** `.skip` / `.fails` / `.todo`, never weaken an assertion to get
  green, never add retries around flakiness — fix the root cause.
- URIs in server replies are validated strictly (see
  `@clice/tools/snapshot` normalizeFileUri). Do not "normalize away" a
  malformed URI; a raw path or unencoded space is a server bug.
- `allowAnomaly` requires the test to assert the expected anomaly itself.
- Comments: `///` for doc comments, `//` inline; explain constraints the
  code can't show, nothing else.
- Same-workspace exclusivity across files comes from the session lock —
  never touch `tests/data/*` outside a session, and never run two suites
  concurrently.

## Known pitfalls (each cost a real debugging session)

- JS numbers mangle 64-bit values: cache.json dep hashes and agentic
  symbolIds need `JSON.rawJSON`/BigInt-reviver round trips (see
  persistent_cache.test.ts, agentic/rpc.ts).
- Python-style truthiness does not port: `expect([]).toBeFalsy()` fails —
  assert `length` explicitly.
- LSP positions are UTF-16 code units; ASCII fixtures keep them equal to
  string indices — non-ASCII fixtures need real conversion.
- `Diagnostic.message` is `string | MarkupContent` — narrow before
  `.includes`.
- Child stdout/stderr backpressure is real: an undrained pipe blocks the
  server; `spawnSync` has a 1MB default `maxBuffer` that silently kills
  children.
