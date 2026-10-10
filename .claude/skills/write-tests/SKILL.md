---
name: write-tests
description: How to write clice integration tests (TypeScript/vitest) — fixture forms, Workspace/CliceClient API, snapshot workflow, hard rules and known pitfalls. Read BEFORE writing or modifying anything under tests/.
---

# Writing clice integration tests

The suite is TypeScript on vitest. Harness = the `@clice/tools` workspace
package (`tools/`, session machinery in `tools/client/session.ts`); each suite binds it in its own fixture file (`tests/integration/fixtures.ts`, `tests/snap/fixtures.ts`). Tests live in
`tests/integration/serve/<area>/*.test.ts` (cases that drive a server) and
`tests/integration/<area>/*.test.ts` (CLI cases); tests of the tooling itself in
`tests/tools/`. Run: `cd tests && CLICE_EXECUTABLE=../build/RelWithDebInfo/bin/bin/clice npx vitest run --config integration/vitest.config.ts <file>`;
gates: `npm run check` at the repo root (tsc strict + ESLint, zero tolerance).

## Choosing a fixture form

1. **A case that drives a server** goes under `tests/integration/serve/<area>/`
   and is written with the serve action layer (`@clice/tools/actions`,
   `tools/client/actions.ts`; its own tests are
   `serve/server/actions.test.ts`). Each case gets a fresh copy of its
   workspace and its own server; teardown is automatic.

   ```ts
   import { at, expect, serve } from "../../fixtures.ts";

   serve.files({ "main.cpp": "int main() { return missing; }\n" })(
     "edit clears the error",
     async ({ s }) => {
       expect(await s.errors("main.cpp")).toHaveLength(1);
       s.edit("main.cpp", { replace: "missing", with: "0" });
       await s.clean("main.cpp");
       await s.indexed();
       expect(s.show(await s.hover(at("main.cpp", "ma|in")))).toContain("main");
     },
   );
   ```

   - Workspaces: `serve(project)` / `serve.each([projects])` copy a sample
     project of `samples/` (its `project.json` names the units, their
     arguments and logical file names); `serve.files({path: text})` lays out
     loose files, every source a unit unless `manifest` says otherwise;
     `serve.data(name)` copies `tests/data/<name>` (a bridge until the data
     workspaces become sample projects).
   - Options: `config`, `manifest` (units of several configurations;
     `${workspace}` in an argument), `databases` (none, elsewhere, several),
     `files` as text or `(workspace) => text`, `setup(workspace)` (symlinks
     and the like), `launch` (capabilities, folders, args, executable,
     config, `beforeInitialized(s)`, `handshake: false`), `env`,
     `anomalies`, `killOn`, `crashOn`. `.skipIf(cond)` and `.for(table)` on
     the test function.
   - Positions and edits name unique snippets: `at(file, "snip|pet")`,
     `{ replace, with }`, `{ after | before, insert }`, `{ remove }`,
     `{ text }` — never a line or column.
   - Every wait ends on a server event: `s.sync()` / `s.indexed()`
     (clice/internal/sync), a pull (`s.diagnostics`, `s.compiled`,
     `s.clean`, `s.errors`, `s.recompiled`), a reply, a hold (`s.hold`,
     `s.inFlight`: a build's reply parked in the master) or a gate
     (`s.gate`, `s.inWorker`: a request parked in its worker, for a cancel,
     edit or death that must land mid-work). `s.pushed` / `s.pushes` read
     what the server published unasked; `s.counts` / `s.stats` read the
     build counts and gauges.
   - A failing case prints its steps and the work the server still had.
   - `s.client` (the raw `CliceClient`) is for what is raw by design:
     messages before the handshake, malformed or out-of-order protocol,
     worker pids, stderr, log and progress notifications.

2. **CLI-only cases and the hooks' own tests**: the `session` factory — the
   test's resource manager. Everything it vends is reclaimed in teardown
   (shutdown gate, anomaly gate, directory removal); never write
   try/finally cleanup. `session.tmpdir()` (an auto-removed `Workspace`),
   `session.tmp()` (with an un-initialized server), `session.spawn(ws)`.
   Options: `initializationOptions`, `allowAnomaly` (ONLY for tests that
   deliberately crash workers — assert on the anomaly explicitly),
   `drainStderr: false`, `args`, `executable`.

3. **Snap tests** (feature output): don't write assertions at all — add
   a fixture to the corpus `tests/snap/<feature>/` and the snap suite
   (`tests/snap/snap.test.ts`, domain logic in `tools/snap/`) pins the
   reply from the paths its `verify:` mode asks for: inspect
   (`clice inspect`, no server) and server (a real server on a
   materialized throwaway workspace). Position-dependent fixtures carry
   `§(name)` annotations (see `@clice/tools/snap/annotation`). A fixture
   is a single `.cpp` or a subdirectory entered through its `main.cpp` —
   one multi-file unit whose sibling sources (module interfaces, headers)
   belong to it; files carrying markers participate, the rest are
   support. A fixture that documents a capability lives in a section
   directory as `<section>/NN_name.cpp` (or `<section>/NN_unit/main.cpp`):
   the directory is the doc page's generated-region key, the two-digit
   number orders the item within the section. Its header has this exact
   shape:

   ```cpp
   /// # Qualified name
   ///
   /// - status: supported
   /// - issues: clangd#710
   /// - verify: both
   /// - snap: separate
   /// - config: {"show_aka": false}
   /// - diagnostics: expected
   /// - indexing: true
   /// - flags: ["-std=c++23"]
   ///
   /// The hover card shows the enclosing namespace and class scope
   ///
   /// Optional further paragraphs describe reader-visible behavior.

   // snap: Maintainer notes about the harness follow the header directly.
   // snap: Every line in the note repeats the prefix.
   ```

   The name is a noun phrase of at most five words, with no dash details
   or terminal punctuation. The first prose paragraph is the card summary:
   one sentence, capitalized (unless it begins with code), with no terminal
   punctuation. Further prose is reader-facing only: behavior the user can
   observe, never internal class names, source paths, or mechanisms. Put
   harness details — why a fixture is server-only, why inspect and server
   differ, or what a snapshot pins — in the adjacent `// snap:` block
   instead. `issues:` cites only issues that map to the capability and are
   still current design-level items; a stale, version-specific bug report
   ("Clangd 11 RC1 …") is padding, not evidence. The header is
   the first content in the file. Edge-case fixtures without a doc header
   stay at the corpus root, un-numbered; any explanatory prologue there uses
   ordinary `//` comments, not `///`.

   Accept intentional changes with `UPDATE_SNAPSHOTS=1 npm run snap` and
   review the diff like code; a shared-snapshot mismatch on the server side
   is a real divergence, not something to update over.

   Fixture meta (strict — unknown keys are errors, validated by
   `tools/snap/corpus.ts` and `tools/docs/feature.ts`), declared as
   `- key: value` lines in the leading `///` header. The only keys, in fixed
   order, are `status`, `issues`, `verify`, `snap`, `config`, `diagnostics`,
   `indexing`, `flags`; omit unused optional keys without reordering the
   rest. `status` is required and is `supported`, `partial` or `unsupported`.
   `status` and `issues` render into the docs; the rest drive the suites:
   - `verify: both` (default) runs inspect and server; `inspect`/`server`
     runs only that path, which then owns the plain `<name>.snap.yml`.
   - `snap:` relates the two paths of a `verify: both` fixture.
     `shared` (default): byte-identical, one `<name>.snap.yml`.
     `separate` (with a `// snap:` comment explaining why): a genuine
     known difference, pinned as `<name>.inspect.snap.yml` /
     `<name>.server.snap.yml`. `skip`: a known-wrong divergence — the
     fixture runs nowhere and keeps no snapshot until fixed. `skip`
     documents a divergence that predates your change — it is never a
     way to get your own regression past the suite.
   - `config: {...}`: feature-options overlay; the snapshot pins BOTH
     halves (`default:` / `configured:` blocks) on both paths.
   - `diagnostics: expected`: the fixture deliberately does not compile
     cleanly — unexpected diagnostics fail, and so does a clean compile
     under the declaration.
   - `indexing: true`: enables background indexing on the server path
     (off by default for speed).
   - `flags: [...]`: extra compile flags, appended to the corpus-wide
     flags in `tests/snap/<feature>/corpus.json`.

   `UPDATE_SNAPSHOTS=1` updates everything in one run: inspect tests run
   first and own shared bodies; the server side can only update its own
   variants.

   Fixture doc headers feed the generated feature pages, but do not
   regenerate or translate them per edit — that happens once at the end
   of the branch, delegated (the docs skill's "Syncing docs at the end
   of a branch").

## API cheat sheet

`Workspace` (`@clice/tools/workspace`): `path(rel)` `uri(rel)` `write`
`read` `exists` `mkdir` `rm` `writeCDB(files, {extraArgs, std})`
`writeEntries` `generateCDB()` `pinCacheDir()` and cache inspection
(`pchFiles()` `pcmFiles()` `tmpFiles()` `readCacheJson()`). Raw string
path: `ws.root`. Exotic fs ops: `node:fs` + `ws.path(...)`.

`CliceClient` (`@clice/tools/client`, `s.client` in a serve case): typed
requests (`hoverAt` `definitionAt` ... `sync` `stats` `poll` `hold` `gate`),
documents (`open` `change` `changeRange` `save` `close`), raw wire
(`sendRequest(TypeOrMethod, params, token?)`, `sendTogether`,
`onNotification`), records (`publishedDiagnostics` `serverRequests`
`logMessages` `progressEvents`), lifecycle (`shutdown` `killServer`
`assertExitedCleanly` `workerPids`). Custom protocol types live in
`@clice/tools/protocol` — NEVER redeclare them locally (the VSCode
extension shares them).

Timing: no `sleep` or polling loop waits for the server — wait on an event
(above). A file whose mtime must differ gets `s.disk.touch(file, date)`.
What stays is a clock rule the case proves (the crash-retry spacing), with
a constant and a one-line comment.

## Hard rules

- **Never** `.skip` / `.fails` / `.todo`, never weaken an assertion to get
  green, never add retries around flakiness — fix the root cause.
- URIs in server replies are validated strictly (see
  `@clice/tools/snap/snapshot` normalizeFileUri). Do not "normalize away" a
  malformed URI; a raw path or unencoded space is a server bug.
- `allowAnomaly` requires the test to assert the expected anomaly itself.
- Comments: `///` for doc comments, `//` inline; explain constraints the
  code can't show, nothing else. Keep tests concise: descriptive test
  names, no large comment blocks explaining layout or expected behavior.
- Cases never run on `tests/data/*` or `samples/*` in place: the
  fixtures copy them. Probes and experiments copy a workspace to a temp
  dir first; a server under test whose `.clice` gets deleted underneath
  it fails every PCH build.
- Tear down servers by the PIDs you recorded (and their subtree) — never
  `pkill` by name. A pattern sweep kills a sibling run's servers, and those
  deaths look exactly like the bug being hunted.

## Known pitfalls (each cost a real debugging session)

- JS numbers mangle 64-bit values: cache.json dep hashes need
  `JSON.rawJSON`/BigInt-reviver round trips (see persistent_cache.test.ts);
  `clice query` answers carry symbol ids as `#<hex>` strings for this reason.
- Python-style truthiness does not port: `expect([]).toBeFalsy()` fails —
  assert `length` explicitly.
- LSP positions are UTF-16 code units; ASCII fixtures keep them equal to
  string indices — non-ASCII fixtures need real conversion.
- `Diagnostic.message` is `string | MarkupContent` — narrow before
  `.includes`.
- Child stdout/stderr backpressure is real: an undrained pipe blocks the
  server; `spawnSync` has a 1MB default `maxBuffer` that silently kills
  children.
- Every snap corpus directory carries a `.clang-format` with
  `DisableFormat: true`. Without it `pixi run format` wraps long `///`
  header lines (meta keys vanish silently) and reflows the code (every
  snapshot drifts).
- `§` is always a marker: a literal `(` right after it is written `§()`,
  and fixture comments never contain a literal `§` — a full-dump canary once
  collapsed to one line because of a `§` in its own comment.
- Include-completion fixtures use a corpus-unique include prefix: the pixi
  env's `$PREFIX/include` leaks into the search path and `-nostdinc` does
  not stop it. Completion statements end with `;` — an unterminated
  statement drags the next marker into recovery context.
- Windows file timestamps tick at ~15.6 ms: two touches inside one tick
  stat equal, so a test that needs distinct mtimes sets them explicitly.
- LMDB reads come from a resident snapshot: `advance` after a write before
  expecting visibility, and a test double wrapping `BlobDatabase` must
  forward `advance`/`retire`/`grow` or the snapshot never moves. Probe blob
  keys go through the path pool's canonical spelling (Windows 8.3 short
  names hash differently).
- `workspaceSymbols` never lists a declaration-only name: `search_symbols`
  skips symbols without a definition, so an index check needs a defined one.
- Tester's module compile once wrote no BMI at all (the syntax-only
  overload cleared the output file) and module tests stayed green on
  lexical tokens alone — when touching the Tester compile path, check that
  a PCM is really produced.
- Under `-ffreestanding` clang recognizes no library builtins
  (`getBuiltinID` is 0): logic keyed on builtin recognition behaves
  differently in unit tests than under a hosted `clice inspect`.
- A unit test runs in zest's worker process: one that crashes is reported
  CRASHED under its own name, with what it printed, and the run goes on.
  A debugger attached to `unit_tests` sees only the runner — take a
  backtrace with `--no-isolation --test-filter=Suite.Case`, which runs the
  test in-process (`lldb --batch -o run -k "bt 40" -- ...`).
- The Tester's driver is the `clang++` on PATH: the cross-built legs
  (arm64 Linux, x64 macOS, both Windows; `build_os` in `test.yml`) run
  their suites with only the `test-run` env active, so on arm64 Linux they compile against the
  runner's system libstdc++, not conda's, and on Windows with the
  runner's clang, which targets MSVC. A wrapper script named `clang++` that execs the
  pixi clang with another `--gcc-install-dir` reproduces such a leg locally.

## C++ unit tests (zest)

- zest constructs a fresh suite object for every `ZEST_CASE`: no `reset()`
  / `clear()` helpers, shared initialization goes in the suite's
  constructor (`ZEST_SUITE(Name)` declares `struct NameTEST`). A
  default-constructed `Workspace` already carries the real configuration
  defaults (`Config` guarantees it; `BornValidDefaults` pins it) — no
  defaults initialization is needed.
- A check takes one expression and shows the operands of its top-level
  comparison: `ZEXPECT(a == b)`, `ZASSERT(result)`. `&&`, `||`, bitwise
  operators and shifts at the top level do not compile — split the check,
  or parenthesize it to check one bool.
- A failed `ZASSERT` ends the test's worker process at once, without
  unwinding: it works the same in a coroutine, a helper or another thread,
  but the suite's destructor does not run — cleanup that must happen then
  goes in a `kota::zest::FatalHook`. Under `--no-isolation` it ends the
  whole run.
- Filter with `--test-filter=<name>`; a bare positional name exits 1
  silently.
- `select("m")` looks up a point while `§(m)⟦...⟧` registers only a range:
  a test that needs both annotates both (`§(m)⟦§(m)foo⟧`).

## Tooling code (`tools/`)

- Framework > mature library > handwritten: vitest's own concurrency
  (`test.concurrent`, `maxConcurrency`) over a custom pool, `jsdiff` over a
  hand-rolled diff, `util.parseArgs` over a custom argv parser; say why a
  library is trustworthy (downloads, maintenance, already in the tree). The
  one exception is byte-level twin code — `yamlStr` mirroring the C++ zest
  escaping, the C++/TS annotation parsers — whose contract is byte identity
  and which a library would break.
- `tools/` runs under node's strip-only TypeScript: erasable syntax only (no
  constructor parameter properties, no enums).
- Naming: no abbreviated file names, no decorative section comments. New
  modules join the subsystem directory they belong to (`tools/client`,
  `tools/snap`, `tools/docs`, `tools/protocol`, each a subpath export of
  `tools/package.json`); a new directory needs a new subsystem, not a new
  file. Framework and domain logic live in `tools/`; test directories keep
  only what must be there (package.json, tsconfig, thin vitest glue).
