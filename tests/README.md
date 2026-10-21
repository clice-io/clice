# tests

The test system clice ships its first release with. The old suites under
`tests/` (`integration/`, `snap/`, `smoke/`) migrated into it and are gone;
this directory takes the name `tests/` next. Until then the coverage check
(below) runs only by hand.

This file is the contract: how tests are organized, what a case is made of, how
it asserts, how the docs come out of it, and what tests never do.

## Cases

Every test lives in a **case**: a directory that is a small project of its own
plus the files that say what to check.

```
tests/<topic>/<subtopic>/NN_name/
  inputs…         any files: sources, headers, clice.toml, .clang-tidy, data
  project.jsonc    optional: how the inputs compile
  test.ts         optional: what to check
  README.md       optional: the case's page in the docs
  snap.toml       every product output the case pins
```

- **Two levels, always.** A topic is a product area (the list below), a
  subtopic one aspect of it. Case directories are numbered (`01_name`, two
  digits, lowercase, `_` between words); the number orders the cases, gaps are
  fine.
- **Self-contained.** A case reads nothing outside its directory but the
  `project.jsonc` and `test.ts` of its subtopic and topic. Nothing is shared
  between cases; a structure two cases need is in both.
- **The entry is `main.*`** (`main.cpp`, `main.c`, `main.cppm`) when a case has
  one file the check is about; other files keep the names their content needs.
- Source files carry only code and `§` markers (below). Documentation goes to
  `README.md`, maintainer notes to `//` comments in `test.ts` or the sources.

### Topics

- **Features**, what a feature answers on code and how its answers behave
  over time: `hover`, `completion`, `signature_help`, `navigation`,
  `document_highlight`, `document_symbol`, `workspace_symbol`,
  `document_links`, `folding_range`, `selection_range`, `semantic_tokens`,
  `inlay_hints`, `code_actions`, `switch_source_header`, `rename`,
  `diagnostics`, `inactive_regions`.
- **Engine**, how the server behaves over time: `compile` (pch, preamble,
  cache, staleness, flags, languages), `contexts` (header contexts, switching,
  self containment, fragments), `project` (discovery, rules, configurations,
  unlisted files, spellings, file tracking, folders, guidance), `index`
  (background, staleness, transfer, freshness, read only, interruption,
  delegation, batch), `modules`, `config`, `protocol` (basics, initialize,
  edges, positions, cancellation, stale results), `lifecycle` (crashes,
  workers, termination, memory, stderr, test hooks, editor sessions).
- **Commands**, what a command prints, writes and exits with: `query`,
  `refactor`, `format`, `lint`, `index`, `analyze`, `modularize`, `inspect`,
  `command`, `scan`, and `cli` for process behavior every command shares.

`tools/testing/cases/topics.ts` lists them by family.

Topics divide by product area, not by the boundary a case checks it through:
`format` holds the command and the editor's formatting, `index` the
background index and `clice index`. A subtopic names an aspect of its topic,
never the topic again or the way a case tests it. A feature topic's own
`test.ts` asks its feature the usual way, at every marker or of the entry
file, so most of its cases bring code and nothing else.

Outside the topics: `editors/`, `nightly/` and `compat/`, each below;
`tools/testing/cases/topics.ts` lists every directory `tests/` may hold, and
any other is an error. C++ unit tests are not
cases: they live in `unittests/` at the repository root.

## Inputs

- **`project.jsonc`** says how the inputs compile; one may sit in the topic, the
  subtopic and the case, merged in that order (the case's last): lists add up,
  the rest is replaced. Comments say why the flags are what they are.

  ```jsonc
  {
    // The target is pinned: hover shows sizes, which differ on LLP64 hosts.
    "flags": ["-std=c++23", "-Iinclude", "--target=x86_64-unknown-linux-gnu"],
    "units": {
      "main.cpp": ["-DHOST"],
      "dual.c": [
        ["-x", "c++", "-std=c++23"],
        ["-x", "c", "-std=c17"],
      ],
    },
    "databases": { "build": ["main.cpp"] },
    "compiles": false,
  }
  ```

  - A command is `clang++` (`clang` for a `.c` unit), `flags`, the unit's own
    arguments, `-fsyntax-only`, the file. Nothing else is added: a topic
    states its standard in `flags`, and a unit's own `-std` overrides it.
  - Without `units`, every C-family source of the case is a unit with no
    arguments of its own; with it, exactly those files are, a list of lists
    giving one unit several commands.
  - Every database's `directory` is the copy's root: relative paths in flags
    resolve there.
  - `databases` writes a `compile_commands.json` into each named directory for
    the units it lists; `{}` writes none (the case ships its own, or none is
    the point). Without it, one database at the root holds every unit.
  - `compiles: false` says the code does not compile on purpose. An answers
    test checks it both ways: errors fail it without the key, a clean compile
    fails it with.

- **A file named `*.in`** is copied without the suffix, `${WS}` replaced by the
  copy's root: a hand-written database or response file that needs absolute
  paths. It carries no markers.
- **Generated inputs** (many units, a large file) come from
  `t.disk.write(generate.units(18))`, never from loops in a test.
- **Every test runs on a fresh copy** of the case's inputs. The project's
  databases are written before the first server or command sees it, unless
  the test wrote one (`t.disk.database()`). Nothing a test does reaches the
  repository or another test.
- **The copy's root is named past ASCII** (`clice-case-é中😀-…`, characters
  of two, three and four UTF-8 bytes), as a home or project directory often
  is: every case runs under such a workspace, cache and log directory on
  every platform. A case about names past ASCII inside the workspace still
  writes them itself.

### Positions

A test names a position by text, never by line and column:

- A marker in a source: `§(name)` a point, `§(name)⟦text⟧` a range, `§` and
  `§⟦…⟧` unnamed, which are named `nameless_0`, `nameless_1`, … across the
  case in source order. A name is unique in its case. Markers are stripped
  before anything compiles, and the framework tells `clice inspect` where they
  were.
  A literal `§` never appears in case text. `t.marker(name)` holds while its
  file reads as the case wrote it; after an edit it fails, and `at` names the
  place. `t.markers()` lists them in path and source order.
- An anchor in a test: `t.at("main.cpp", "shapes::|area(")`, the unique place
  the text occurs, the cursor at `|`. A missing or ambiguous anchor fails the
  test.

## test.ts

```ts
import { answers, test } from "@clice/tools/testing";

answers(async (a) => {
  for (const m of a.markers()) {
    await a.show(m.name, a.hover(m));
  }
});

test("buffer edit reaches the answer", async (t) => {
  const s = await t.start();
  await t.show("before", s.hover(t.marker("use")));
  s.edit("main.cpp", { replace: "int value", with: "long value" });
  await t.show("after", s.hover(t.at("main.cpp", "long |value")));
});
```

A script registers tests of two kinds, each `(fn)`, `(name, fn)`,
`(options, fn)` or `(name, options, fn)`:

- **`answers`**: what clients answer about the code as the case wrote it. It
  runs twice, on `clice inspect` and on a real server, each over its own copy,
  and the two must show equal text. `a` asks what `s` asks — `a.hover(m)`,
  `a.completion(m)`, `a.codeActions(m, { compile: true })`,
  `a.semanticTokens(file | m)`, `a.inlayHints(file | m)`,
  `a.documentSymbols(file)`, `a.definition(m)`, `a.workspaceSymbols(query)`,
  … — and `a.markers()` lists the case's markers in source order. A request
  one client cannot answer fails at once: the test declares `paths`. An
  answers test only reads; `compile: true` checks each action's edits and
  restores the files.
- **`test`**: everything else, on servers, the command line, or both.
  `t.start(options?)` starts a server on the copy; `t.cli(...)` runs a command
  there (with `interrupt: { units, signals }` it signals the run once that many
  units are indexed); `t.disk` reads and changes the copy (`write`, `edit`,
  `rm`, `symlink`, `touch`, `list`, `corrupt`, `database`); `t.replay()` plays
  the case's `trace.jsonl` to a server; `t.gone(pids)` waits until processes
  the test did not start have ended; `t.elapse(ms, why)` lets time pass
  where time is the claim. A test may start several servers and run several
  commands; each is shut down and gated at the end.

**The clients.** `clice inspect` reads one source file at a time, compiled
alone with its databases' command: a module importer has no inspect answer.
Its first request of a feature at a marker asks at every marker of the file
in one run. The server opens every source, module interfaces first and the
entry last, then every marked header, before the script asks. Both check the
code compiles cleanly (`compiles: false` says it must not), inspect every
source but an importer and every file it read, the server every file it
opened. The server's client sends URIs as an editor does, percent-encoded
(`file:///c%3A/…` on Windows), and compares them decoded.

**Diagnostics and parked work.** `s.diagnostics(file)` opens the file if
needed and pulls its diagnostics, `s.errors(file)` keeps the errors,
`s.pushes(file)` lists every push it received once the server settled.
`s.park(work, file, { at: "reply" | "worker" })` holds the next `work` on
the file at its reply or where it starts in its worker until the test
releases it, so a cancel, an edit or a death lands mid-work; a park left
unreleased goes with the server.

**Server options**, given to `t.start` and kept by every restart of that
server: `config` (initializationOptions in clice.toml's shape), `capabilities`,
`folders`, `args`, `env`; and the faults the test provokes on purpose —
`crash: { request, file }` crashes every such request's worker
(`CLICE_TEST_CRASH_REQUEST`), `kill: { request, file }` kills the first one's
from outside (`CLICE_TEST_KILL_REQUEST`), `anomalies: true` misleads the
server and asserts the anomalies itself. A crash, kill or anomaly nothing
declared fails the test, and so does a declared one that never happened.

**Test options**:

| Option                     | Means                                                                                             |
| -------------------------- | ------------------------------------------------------------------------------------------------- |
| `except: { win32: "…" }`   | platforms the test has no meaning on, each with the reason; there it is listed as not applicable  |
| `errors: ["…"]`            | the error-level log lines the test provokes; any other fails                                      |
| `paths: { server: "…" }`   | (answers) one client alone, where the other cannot answer; `inspect` likewise                     |
| `paths: { separate: "…" }` | (answers) both clients pinned apart where they answer differently on purpose; agreeing then fails |
| `config: { … }`            | (answers) options in clice.toml's shape, taken by both clients                                    |
| `indexing: true`           | (answers) the server indexes first                                                                |

**Recorded sessions.** A `trace.jsonl` is what a real editor sent a server,
recorded by `tools/testing/server/record.ts` standing in for the server it
launches. `t.replay()` plays it at the recorded pace on the copy, the recorded
root moved there, against the server's real defaults: every request must be
answered, an error only to one the editor cancelled or whose document changed
under it; teardown gates the exit. Traces are re-recorded, never edited; they
live in `lifecycle/editor_sessions/`.

Tests in one script share nothing but the case's inputs. Names are short and
unique in the script; one unnamed test is allowed when it is the only one.

**Inherited scripts.** A case without `test.ts` takes its subtopic's, then its
topic's. A script sits at the lowest directory every case running it shares: a scenario unique to a case is its own `test.ts`; one a
subtopic's cases all run on their different projects is written once, as
`compile/cache/test.ts` restarts a server over each case's project and
`lifecycle/editor_sessions/test.ts` replays each case's trace. A case with no
script anywhere fails, and so does a script above the cases that none of them
runs.

## Output

`snap.toml` is the case's output: per test, every `show` and every command's
transcript in run order, each a block, the table `[<test>.<label>]`. A test
is named by its script, an unnamed one by its kind (`answers`, `test`).

```toml
[answers.local]
highlights = [
    { at = "int ⟦remaining⟧ = start;", kind = "text" },
    { at = "⟦remaining⟧ = remaining - 1;", kind = "write" },
]

[answers.after.hover]
markdown = '''
### macro `LIMIT`
'''
range = "int capacity = ⟦LIMIT⟧;"

[test."$ clice format main.cpp"]
exit = 0

[test."$ clice format main.cpp".files]
"main.cpp" = '''
-int  x;
+int x;
'''
```

- **Blocks.** A label is what the script names it, a marker's name or a
  file's path as a rule, unique in its test. The file is TOML 1.1: a list
  or table stays on a line of 120 columns, else takes a line per item, as
  does a list of tables or lists; text of several lines (markdown, diffs,
  a command's output) is a raw `'''` string, unescaped; a table follows as
  `[…]` of its own.
- **`show(label, value, options?)`** takes a request itself, `s.hover(at)`,
  which knows what it asked: its answer goes under the request's kind
  (`hover`, `highlights`, `definition`, …) as the table below shapes it. A
  record of requests, `{ definition: a.definition(m), declaration: … }`,
  puts each answer under its own label; text, lines or a number go under
  `value`. `parts` picks an answer's optional parts (a completion's
  `insert`, `filter`, `edit`, `edits`, `documentation`; a symbol's `detail`, `range`; a hint's `kind`,
  `padding`, `links`). Everything shown reads the copy's root `${WS}`, a
  worker's cause of death `${CRASH}`.
- **Commands** are a block under the command line, `$ clice …`: `stdout`
  (JSON pretty-printed), `stderr` (a log line only at warning and above, as
  `warning: …`), `exit` (or `killed` and the signal), and `files`, each file
  the command wrote as a diff. A command run again in one test is `#2`.
  Paths read `${WS}/…`, a session's log directory `${SESSION}`, a worker's
  cause of death `${CRASH}`, durations and sizes `#ms`, `# KB`, and in
  JSON the number under a duration's or a size's key (`seconds`, `…Ms`,
  `…Bytes`) `#`.
- **Paths apart.** An answers test whose clients answer differently on
  purpose (`paths: { separate }`) has an `inspect` and a `server` block,
  each holding the blocks of its client.
- **Hand-written checks are for state** (`expect(counts.pch).toBe(0)`,
  equality of two observations, clean or failing compiles), through vitest's
  `expect`. An upper bound on builds is a state check only for a claim that
  something settles rather than loops, with a comment on where the bound
  comes from.
- **Banned**: writing product output in a test, or matching it partially —
  `toContain` on an answer, `toBeGreaterThan(0)` on a result count, a regular
  expression over a summary line. Either the whole answer is in the output,
  or the claim is an exact state check.
- **Known wrong.** A block whose current output is wrong carries the reason
  in `snap.toml`, written for a reader of the docs:
  `wrong = "clang 22 drops it (clangd#696)"`. Every update keeps it while the
  block exists. A case's status in the docs follows its flags: none
  `supported`, some `partial`, all `unsupported`. What clice answers on code
  the standard makes ill-formed with no diagnostic required is never fixed:
  a case pins it, flagged known wrong.
- **Updating.** `UPDATE_SNAPSHOTS=1` rewrites every output and prints, per
  case, the blocks whose output changed — those flagged known wrong (likely
  fixed) apart from the rest (likely regressed) — and the blocks added and
  removed. A pull request carries that summary.

### Rendering

Product answers are values a person reads the way an editor shows them.
Line and column numbers never appear.

A position or range renders by what is there:

1. It is a range a marker named: the name.
2. Otherwise its line, the range set off in `⟦` `⟧`, indentation dropped:
   `double ⟦area⟧(const Circle& circle);`.
3. In another file than the query's, the file first:
   `include/circle.h │ double ⟦area⟧(const Circle& circle);`; outside the
   workspace under `${SYSROOT}`, `${RESOURCE}` or its absolute path.
4. Spanning lines, the first and last: `⟦int watch(int v) { … }⟧ (4 lines)`.
5. Empty: `⟦⟧` at its place.
6. When the line's text is not unique in its file, an offset follows: `@-9`
   from the query line in the same file, `@12` (1-based) elsewhere.

Every rendering resolves back to exactly the range it came from. The docs do
not read it back: the run that checks a documented case writes the case's
card with each place's range (below).

| Answer (kind)                                                                                                  | Shape                                                                                                                                                                                                                            |
| -------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| locations (`definition`, `declaration`, `type_definition`, `implementation`, `references`), `selection_ranges` | positions, sorted; selection ranges innermost first                                                                                                                                                                              |
| `highlights`                                                                                                   | `{ at, kind }`, kind `read`, `write` or `text`                                                                                                                                                                                   |
| `incoming_calls`, `outgoing_calls`, `supertypes`, `subtypes`                                                   | `{ name, kind, at }`; calls add their `sites`                                                                                                                                                                                    |
| `hover`                                                                                                        | `markdown` as the client receives it and the symbol's `range`; no card is `{}`                                                                                                                                                   |
| `completion`                                                                                                   | `items`, the first ten by rank: `label`, `kind`, `detail`; `more` counts the rest (the clients agree on all); `incomplete` when the server sent only some. Parts: `insert`, `filter`, `edit`, the other `edits`, `documentation` |
| `signature_help`                                                                                               | `signatures`, the active parameter in `⟦⟧`, the `active` one and its `documentation`                                                                                                                                             |
| `inlay_hints`                                                                                                  | each line with its hints in place, `draw(⟨width:⟩ 800)`. Parts list the line's `hints` with `kind`, `padding`, `links`                                                                                                           |
| `semantic_tokens`, `semantic_token`                                                                            | of a file every token, `{ at, type, modifiers }`; at a place the token's `text` and `type`, or the word there and `none`                                                                                                         |
| `contexts`                                                                                                     | `{ label, description, host }` per context                                                                                                                                                                                       |
| `folding_ranges`                                                                                               | `{ kind, at, collapsed }`                                                                                                                                                                                                        |
| `document_symbols`                                                                                             | the outline, `{ name, kind, at }` with `children`. Parts: `detail`, `range`                                                                                                                                                      |
| `document_links`                                                                                               | `{ at, target }`                                                                                                                                                                                                                 |
| `diagnostics`, `errors`, `pushes`                                                                              | `{ at, severity, message, source, code }`; pushes a list per push                                                                                                                                                                |
| edits (`code_actions`, rename, formatting)                                                                     | each changed file's diff with two lines of context, trailing whitespace shown `·`; an action has `kind`, `title`, its `edits`, and checked, whether it `compiles`                                                                |
| `workspace_symbols`                                                                                            | `{ name, kind, at, container }`                                                                                                                                                                                                  |
| `counterparts`                                                                                                 | the `candidates`, best first, each with its `reasons`, and the `preferred` one                                                                                                                                                   |

## Docs

The docs are generated from cases and checked in CI; nothing in them is
written by hand that a case can show.

- **`README.md`** makes a case documented, wherever it lives:

  ```markdown
  ---
  issues: [clangd#710]
  ---

  # Use before definition

  Hovering a macro before its definition shows nothing yet

  Further paragraphs describe what a user observes.
  ```

  The `---` block is optional and holds `issues` and `option`. The title is at
  most five words; the summary is one sentence, no final period. Prose is for
  readers: no internal names, no mechanics. A case that pins an edge or a bug
  once fixed has no README: it sits in the subtopic of the aspect it pins and
  never reaches the docs.

- **Feature pages** keep their own text: what the feature is, each
  section's heading and lead, the order of the sections, which features
  share a page. A section's cards fill its generated region, keyed
  `<!-- BEGIN GENERATED ITEMS: <topic>/<subtopic> -->`
  (`tools/testing/docs/site.ts`): each documented case of the subtopic, in
  case order, a card with its title, status, summary and prose. Status and
  `issues` feed the support matrix.
- **Cards.** Each documented case has a card, `docs/cards/<id>.json`: its
  page, its sources without markers and the markers' offsets, each test's
  `config`, and its output with every place's range, so the site draws the
  answers into the code without reading `snap.toml`. Every run checks the
  card as it checks `snap.toml`, and `UPDATE_SNAPSHOTS=1` rewrites both. A
  case's sources are what the card shows: maintainer notes go in its
  `test.ts`.
- **Checks.** `pixi run check-feature-docs` fails where pages, cases and cards
  disagree: a stale region, a documented subtopic on no page, a card no case
  owns; `pixi run update-feature-docs` rewrites the regions and drops the
  orphan cards.
- **Command pages**: documented cases of a command topic are its examples, in
  order: the commands and their transcripts.
- **Configuration page**: a documented case whose `---` block names
  `option: rules.append` is that option's example, wherever it lives.

## Waiting and teardown

- **Waiting** happens only on product events: a reply, the diagnostics of a
  version, `s.sync()` (the server idle, exact accounting), a park reached, a
  command's exit, a batch pause point reached. No sleeps, no polling,
  no wall-clock assertions; where time is the claim, `t.elapse(ms, why)`.
  The one end with no event, that of a process the test did not start, is
  `t.gone(pids)`, which looks for the processes until they are gone. The
  lint rule `clice/timers` (tools/lint) holds every timer in tools/, tests/
  and editors/vscode to this, `pixi run check-timers` those of src/ and
  unittests/.
- **Test hooks** make the events observable: the server's `clice/internal/sync`,
  build counts, holds and worker gates (`s.park`), watches of its background
  loops (`clice/internal/watch`, `s.watch`); the batch commands' pause point
  (`CLICE_TEST_PAUSE`), which parks a run after a given unit so a test can
  signal it at a known place. A hook the tests need is added to the product.
- **At teardown**, every test: every server exits cleanly, and the logs hold
  no anomaly, worker crash or error-level line the test did not declare.
- **On failure** a test prints its step tree, what `sync` reports still
  running, and the warning and error lines of every server it started.
- **Time.** `sync` gives up after 4 minutes and says what is left; a script
  still running after 250 s is hung, fails with its steps and is torn down;
  the runner gives up on a test at 5 minutes.

## Coverage

Tests record what they exercise: every method a client sends, every command
line run, every configuration key set. `npm run coverage`, after the other
runs with the same `CLICE_COVERAGE_DIR`, lists every item of the product
surface nothing exercised and fails. The surface comes from the product: the
configuration schema, each command's `--help`, the server's capabilities, the
public `clice/*` requests. An item is `method <name>`, `command <words>`,
`flag <command> --<name>` (`flag * --<name>` for one every command takes) or
`config <key>`; it is exempted only with a reason in `tests/coverage.json`, and
an exemption naming nothing uncovered fails too.

## Outside the topics

- **`editors/zed/`**: the Zed extension in a real Zed (`pixi run zed-e2e`): it
  downloads clice, which starts with its builtin headers, for C, C++ and CUDA
  files alike.
- **`editors/NN_name/`**: a project and the scenario every editor client runs on
  it (`scenario.json`: the file, the symbol, where it is defined, the header
  context); each client's harness reads it. Until then the editors'
  end-to-end tests (`pixi run -e editor editor-test`) run on copies of three
  cases, `lifecycle/editor_sessions/01_hello_world`,
  `modules/imports/01_imported_symbol` and `contexts/switching/01_two_hosts`
  (`tools/testing/cases/copy.ts`), each harness naming the file and symbol
  in a scenario table of its own: changing one of these cases changes those
  tests.
- **`nightly/projects.json`**: real projects, each pinned to a commit (clice
  itself at the commit under test) with how its compilation database is made,
  a project too large for one runner split into parts. Every night a free
  runner per project or part and platform (Linux, macOS, Windows; a project
  names the ones it builds on) (`nightly-projects.yml`) prepares it, indexes it
  from a cold cache and asks the features on sampled sources: inspect and a
  server the same requests at the same places, the server also navigation;
  headers those sources include are asked of the server alone, which compiles
  one inside a unit including it.
  The job fails on a preparation that fails, a crash, a hang, an index run
  that does not finish, an anomaly in clice's logs, a request either answers
  with an error, a unit clice fails or reports errors on that the compiler
  compiles (or that the compiler cannot check), and a place where inspect and
  the server answer differently; a unit the compiler rejects too is listed, and
  so is a difference a case pins as accepted.
  Counts, sizes, memory and time are recorded beside the previous night's,
  never gated. Every failure names the command that reproduces it:
  `node tools/nightly/run.ts <job> --dir <dir>` prepares the project in
  `<dir>` (clice in the repository itself) and runs the job, `--steps`,
  `--file` and `--only` narrow it.
- **`compat/`**: real build systems and compilers build a small project; clice
  must read their databases and agree with the compiler.

## Unit tests

`unittests/` at the repository root holds the C++ tests of what no product
boundary observes cheaply, built with the sources. Names of at most four words.

- **No input files.** A unit test's inputs are in its code (the `Tester`'s
  sources, a layout it writes to a temporary directory); it reads nothing
  checked in. System headers a compile finds are not inputs.
- **Files belong to cases.** A component whose behavior is best shown on real
  source files is exposed through `clice inspect` and tested as cases, when
  its output reads well as text: the template resolver's resolved types are a
  diff a reviewer can judge. A check whose only result is pass or fail (a
  hash, an invariant) stays a unit test.
- A feature's answer is a feature case, not a unit test.

## Budgets

- Pull-request CI under 20 minutes wall clock; no job over 10 minutes on the
  slowest leg.
- A test under 30 seconds in Debug on four cores.
- The nightly run fits the free runners.

## Never

- Pin every feature on every construct: breadth is the nightly run's.
- Write product output by hand, or match it partially.
- Wait on, or assert about, wall-clock time.
- Retry, skip or weaken a failing test.
- Share inputs between cases.
- Hand-write or edit LSP traces.
- Put agent-driven editor sessions in CI.
- Gate on a coverage percentage.

## Running

```
pixi run cases-test [type] [i/n]   every case, or shard i of n, against build/<type>
npm run cases -w tests        every case, against $CLICE_EXECUTABLE
CLICE_CASES=hover/ …               the cases under a path
CLICE_CASES_SHARD=2/4 …            shard 2 of 4: whole subtopics, balanced by case count
UPDATE_SNAPSHOTS=1 …               rewrite outputs, print the summary
CLICE_TEST_JITTER=50 …             random delays up to 50 ms where the server's own work
                                   races a test's actions: an output that changes left
                                   an order unpinned
npm run coverage -w tests     after a full run
npm test -w tools                  the tools' own tests, the framework's among them
```

Commands run under the pixi environment. Repository formatting skips case
inputs (`.fdignore`, `tombi.toml`): they are test data, formatted the way the
case needs them, and `snap.toml` the way the framework writes it.

## Framework

```
tools/testing/
  cases/          discovery, project.jsonc, copies, scripts, answers tests, snap.toml
  render/         the requests both clients answer, answers to text and back to ranges
  server/         the server client and its actions over the test hooks
  inspect/        the clice inspect client
  cli/            command runs and transcripts
  coverage/       surface and exercise records
  logs/           failure reports and the error gate
  docs/           pages from documented cases
  runner/         the vitest entry points over tests/
  **/*.test.ts    the framework's own tests, beside the code
```
