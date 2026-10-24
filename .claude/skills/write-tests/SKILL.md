---
name: write-tests
description: Read before writing or modifying anything under tests/ or unittests/, or the tools' own tests — the case workflow, unit tests, hard rules and known pitfalls.
---

# Writing clice tests

Every test but the C++ unit tests is a **case**: a directory
`tests/<topic>/<subtopic>/NN_name/` that is a small project of its own,
with the `test.ts` that says what to check (its own or inherited from its
subtopic or topic), a `README.md` when it belongs in the docs, and
`snap.toml`, every output it pins. **`tests/README.md` is the
contract**: topics, inputs and markers, the two kinds of test (`answers`,
`test`), the clients, outputs and how they render, docs, waiting and
teardown. Read it before writing a case; this skill adds only the workflow,
the hard rules and the pitfalls. The framework is `tools/testing/`
(`@clice/tools/testing`). `tests/` keeps the compat and Zed suites.

## Workflow

1. Place the case by product area, not by the way it checks: the topic and
   subtopic of the behavior (`tools/testing/cases/topics.ts` lists the
   topics). Copy the shape of 2-3 neighboring cases; one project per case,
   shared by several tests.
2. Write the inputs, with `§` markers where the tests ask; a `test.ts` only
   when the inherited script does not fit; a `README.md` only when the case
   shows a capability a reader should see. A case that pins an edge or a
   fixed bug has no README.
3. Run it alone, on the box the build ran on (the test skill):
   `CLICE_CASES=<topic>/<subtopic>/NN_name pixi run -e test-run cases-test`.
   `UPDATE_SNAPSHOTS=1` writes its `snap.toml` (and a documented case's card
   under `docs/cards/`) and prints what changed: review every block like
   code. They are written on the box: `box cp $id . <case dir> docs/cards`
   brings them back before the next `box push` overwrites them. What clice gets wrong stays in the output, with
   `wrong = "<reason>"` on the block.
4. Gate: `pixi run npm run check` at the repo root (strict tsc + ESLint,
   zero tolerance). Page regions and translations are synced, and
   `check-feature-docs` run, once at the end of the branch (the docs skill's
   "Syncing docs at the end of a branch").

## Hard rules

- `.fails`, `.todo` and a weakened assertion or snapshot are skipping a
  test (CLAUDE.md). `except: { <platform>: "why" }` is for a test with no
  meaning on a platform, never for one that fails there.
- An `answers` test whose inspect and server outputs differ shows a real
  divergence: fix it, or declare `paths` with the reason. Never
  `UPDATE_SNAPSHOTS` over it.
- URIs in server replies are validated strictly
  (`tools/testing/render/paths.ts`). Do not "normalize away" a malformed
  URI; a raw path or unencoded space is a server bug.
- Comments: `///` for doc comments, `//` inline; explain constraints the
  code can't show, nothing else. Maintainer notes about a case go in its
  `test.ts`, never in its sources, which the docs show.
- Every test runs on a fresh copy of its case. Probes and experiments work
  on a copy too (`CLICE_KEEP_WORKSPACE=1` keeps a test's copy and prints
  where); never point a server at a case directory itself.
- Tear down servers by the PIDs you recorded (and their subtree) — never
  `pkill` by name. A pattern sweep kills a sibling run's servers, and those
  deaths look exactly like the bug being hunted.

## Known pitfalls (each cost a real debugging session)

- JS numbers mangle 64-bit values: parse such JSON with `JSON.rawJSON` or a
  BigInt reviver; the CLI's answers name symbols by `#<hex>` locator strings
  for this reason.
- Python-style truthiness does not port: `expect([]).toBeFalsy()` fails —
  assert `length` explicitly.
- LSP positions are UTF-16 code units; ASCII sources keep them equal to
  string indices — non-ASCII sources need real conversion.
- `Diagnostic.message` is `string | MarkupContent` — narrow before
  `.includes`.
- Child stdout/stderr backpressure is real: an undrained pipe blocks the
  server; `spawnSync` has a 1MB default `maxBuffer` that silently kills
  children.
- `§` is always a marker: a literal `(` right after an unnamed one is
  written `§()`, and case text never contains a literal `§`, comments
  included.
- Include-completion cases use an include prefix unique to the case: the
  pixi env's `$PREFIX/include` leaks into the search path and `-nostdinc`
  does not stop it. Completion statements end with `;` — an unterminated
  statement drags the next marker into recovery context.
- Windows file timestamps tick at ~15.6 ms: two touches inside one tick
  stat equal, so a test that needs distinct mtimes sets them explicitly.
- LMDB reads come from a resident snapshot: advance it after a write before
  expecting visibility, and a test double wrapping `BlobDatabase` must
  forward `advance_read_snapshot`/`retire_old_snapshot`/`grow` or the
  snapshot never moves. Probe blob
  keys go through the path pool's canonical spelling (Windows 8.3 short
  names hash differently).
- Module tests stay green on lexical tokens alone even when the Tester
  writes no BMI at all: when touching the Tester compile path, check that a
  PCM is really produced.
- Under `-ffreestanding` clang recognizes no library builtins
  (`getBuiltinID` is 0): logic keyed on builtin recognition behaves
  differently in unit tests than under a hosted `clice inspect`.
- A unit test runs in zest's worker process: one that crashes is reported
  CRASHED under its own name, with what it printed, and the run goes on.
  A debugger attached to `unittests` sees only the runner — take a
  backtrace with `--no-isolation --test-filter=Suite.Case`, which runs the
  test in-process (`lldb --batch -o run -k "bt 40" -- ...`).
- The Tester's driver is the `clang++` on PATH: the cross-built legs
  (arm64 Linux, x64 macOS, both Windows; `build_os` in `test.yml`) run
  their suites with only the `test-run` env active, so on arm64 Linux they compile against the
  runner's system libstdc++, not conda's, and on Windows with the
  runner's clang, which targets MSVC. A wrapper script named `clang++` that execs the
  pixi clang with another `--gcc-install-dir` reproduces such a leg locally.

## Answers the index serves

Until a file's AST is current, the server answers its outline from the
index, and both answers must be the same. `clice inspect
document-symbols <file> --check` compiles the file, projects the outline from
its own TU index and lists each entry where the two differ (`only_ast`,
`only_index`, `name`, `kind`, `range`, `parent`, `detail`, `deprecated`) at
its 1-based line and column. When a change touches the outline or the index
facts it reads, run it over real code — the repository's sources, the
libc++ and libstdc++ headers, LLVM's (a header outside every project takes
its command from a `clice.toml` above it whose rule sets `default_command`) —
and pin each difference that matters in a case.

## C++ unit tests (zest)

- Names of at most four words; what belongs in a unit test rather than a
  case is in `tests/README.md`, "Unit tests".
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
  one exception is byte-level twin code — the C++/TS annotation parsers —
  whose contract is byte identity and which a library would break.
- `tools/` runs under node's strip-only TypeScript: erasable syntax only (no
  constructor parameter properties, no enums).
- Naming: no abbreviated file names, no decorative section comments. New
  modules join the subsystem directory they belong to (`tools/testing`,
  `tools/client`, `tools/docs`, `tools/protocol`, `tools/compat`,
  `tools/ecosystem`); a new directory needs a new subsystem, not a new file.
  Framework and domain logic live in `tools/`; test directories keep only
  what must be there (package.json, tsconfig, thin vitest glue).
- The tools' own tests sit beside the code they test as `*.test.ts`;
  `npm test -w tools` runs them all, with no clice build.
