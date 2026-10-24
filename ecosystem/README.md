# ecosystem

clice over real projects, in two kinds of job: the **regression**, which
checks that clice handles a project (no crash, no hang, no error the compiler does not
report, the same answers from `clice inspect` and from a server), and
**perf**, which measures clice against clangd on the same project, the
same files and the same machine. Neither runs on a push: that is `tests/`.

A round runs both over every project. This file is the contract: what a
project directory holds, what each kind of job does and records, what fails
one, and when CI runs a round. The framework is
`tools/ecosystem/`, to this directory what `tools/testing/` is to `tests/`.

## Projects

```
ecosystem/
  test.ts            the default script: a project without a test.ts runs it
  <project>/
    project.json     where the project comes from and how it is prepared
    test.ts          optional: what a round does on it
    README.md        why the project is here: what it covers that no other does
```

One directory per project, named as the project names itself (`llvm`,
`abseil`, `mp-units`). A project built several ways (another compiler, another
build option set) is one directory with several configurations, never a second
project.

### project.json

```jsonc
{
  "about": "20260817.0 with its tests",
  "repository": "https://github.com/abseil/abseil-cpp",
  "commit": "2065f4ded0558c6f89fee67c8e5228feb4eb960e",
  "configurations": {
    "clang": {
      "about": "C++20, built by clang",
      "prepare": [["cmake", "-S", ".", "-B", "build", "-G", "Ninja", "${CMAKE_TOOLCHAIN}", "…"]],
      "database": "build/compile_commands.json",
    },
    "gcc": {
      "about": "C++20, built by Ubuntu's GCC 13: libstdc++ and GCC's driver defaults",
      "platforms": ["linux"],
      "packages": ["g++-13"],
      "prepare": [["cmake", "…", "-DCMAKE_CXX_COMPILER=g++-13"]],
      "database": "build/compile_commands.json",
    },
  },
}
```

- **Source.** `repository` and `commit` (a full hash) fetch that commit alone;
  `archive` (`{ "url", "sha256" }`) downloads and unpacks one; neither is clice
  itself, at the commit under test, in the repository's own checkout.
- **Configuration.** `about` for a reader; `platforms` of `linux`, `macos`,
  `windows` (all when absent); `packages`, the Debian packages its preparation
  needs on Linux; `environment: "vcvars"`, when its compiler is MSVC's `cl`
  and needs Visual Studio's environment (the preparation and the compiler's
  checks get it, clice and clangd never: an editor is not started from it);
  `prepare`, its steps; `database`, the checkout-relative compilation database
  they write; `parts`, below; `keep`, a count of units a database too heavy
  for one runner is thinned to, spread evenly over it in path order.
- **Steps** run in the checkout, in order. An argv, in which `${JOBS}` reads
  the machine's core count, `${CHECKOUT}` the checkout's path with `/`, and an
  argument `${CMAKE_TOOLCHAIN}` the CMake arguments of the platform's usual
  target (MSVC's on Windows, where xclang's default is MinGW); or
  `{ "generated": "<build dir>" }`, the generated sources and headers of a
  CMake build using Ninja, built without the order-only dependencies CMake
  adds (LLVM's TableGen outputs in 11 minutes on 4 cores, not an hour); or
  `{ "database": [entries] }`, a database written as given, for a project with
  no build system to write one (`${CHECKOUT}` expanded).
- **Parts.** A configuration too large for one runner names parts, each the
  units under its checkout-relative prefixes; a unit belongs to the part with
  its longest matching prefix, and every unit to exactly one. A part's job
  writes the part's entries beside the checkout with a `clice.toml` naming
  them, so clice reads that database and no other.
- **Checkouts.** Each configuration prepares a checkout of its own,
  `<dir>/<project>/<configuration>`. A finished preparation leaves a marker
  there; a job run again on the same directory skips the preparation. A
  checkout at another commit is an error, never reused.
- **Jobs**: a regression job is named `<project>[/<configuration>][/<part>]`,
  the configuration left out when the project has only one (`linux`,
  `abseil/gcc`, `llvm/all/flang`); a perf job `<project>/<name>`, by the name
  its script gives it (`abseil/editing`). A job runs on one platform, one CI
  runner each.

A preparation that fails fails its job as the setup's failure, never clice's.

### test.ts

```ts
import { perf, regression } from "@clice/tools/ecosystem";

regression({
  configurations: ["clang", "gcc"],
  known: [
    {
      issue: "10-22#6",
      configuration: "gcc",
      kind: "disagree",
      request: "signatureHelp",
      why: "a declaration and its definition name a parameter differently",
    },
  ],
});

const rawHashSet = {
  file: "absl/container/internal/raw_hash_set_test.cc",
  symbol: {
    after: "TEST(GrowthInfoViewTest, GetGrowthLeft) {",
    at: "growth_info->⟦InitGrowthLeftNoDeleted⟧(/*growth_left=*/5, kCapacity);",
  },
  member: "EXPECT_EQ(growth_info->⟦⟧GetGrowthLeftLowerBound(), 5);",
  template: "return c.⟦⟧common().TombstonesCount();",
};

perf("editing", { configuration: "clang" }, async (p) => {
  await p.coldStart(rawHashSet, { runs: 5 });
  await p.index(rawHashSet);
  await p.warmStart(rawHashSet, { runs: 5 });
  await p.editLoop(rawHashSet, { edits: 20 });
  await p.warmRequests(rawHashSet, { repeats: 30 });
});
```

A script registers the jobs a round runs on the project:

- **`regression(options?)`**: the regression on each of `configurations`
  (default every one), `sample` source files a night (default 40), with the
  project's `known` issues.
- **`perf(name, options, script)`**: one perf job per platform of its
  `configuration` (the only one by default; `platforms` narrows them),
  running `script` (the scenarios, below), with the project's `known` issues
  and the `clangdArgs` clangd takes for what it does only on request (C++20
  modules). A project splits its scenarios into several jobs when one would
  not fit a runner's six hours.

`ecosystem/test.ts` is `regression()`: every project is regression-tested on
every configuration unless its own `test.ts` says otherwise. A project's
`test.ts` replaces it; nothing is inherited.

**Request positions** are written as the target line's code with the range in
`⟦` `⟧`, the way a case's output renders a range (`tools/testing/render`):
`growth_info->⟦InitGrowthLeftNoDeleted⟧(…)` asks at the name's start,
`c.⟦⟧common()` between `.` and `common`. Never a line or column number: a
pinned commit's bump must not move them, and third-party sources carry no
markers. The text, indentation left out, names one line of the file; a line
the file repeats takes `{ after, at }`, the first such line after the one
`after` names. A position naming no line, or several, fails the job.
A file's positions are `symbol` (hover, definition, references, call
hierarchy), `member`, `unqualified` and `template` (completion there) and
`call` (signature help); a request whose position is absent is not asked.

**Known issues** are the project's failures already on the board, each with
its issue id and a `why` for the reader, like a case's `wrong`. One matches the
failures of its job (`configuration`, `part`, `platform` when given) whose
`kind`, `request` and `file` (a path or a pattern) it names. It is as narrow
as its failure: one naming a kind alone would hide the next failure of that
kind.

## The regression

A job, on a cold cache:

1. **Prepare** the configuration and read the job's database.
2. **Index** it all with `clice index`, time, memory and the index's size
   recorded. Every unit clice fails, or reports errors on, is compiled again
   by the project's compiler with its database command, checking the code
   only: a unit the compiler rejects too is the project's, listed, not
   failed.
3. **Sweep** sampled files: the job's `sample` sources the seed picks (by
   default the date; the same seed, the same files) on a runner asking three
   files at once, fewer on one with less memory that asks fewer (a sweep
   takes the same time everywhere), and a quarter as many of the headers the
   job's units include. Each source is asked the same requests at the
   same places by `clice inspect` and by a server over the indexed project,
   the two answers compared: semantic tokens, folding ranges, document
   symbols, document links and inlay hints of the file; hover, highlights and
   selection ranges at 20 identifiers spread over it; completion at 3 member
   accesses and signature help inside 3 calls. The server is also asked
   definition, references, call and type hierarchy at each identifier, which
   only it answers, and the errors the file opens with, confirmed with the
   compiler as the index's are. A header is asked of the server alone, which
   compiles it inside a unit including it; a source importing a module too,
   which inspect cannot compile alone.

A job **fails** on: a preparation that fails; a crash or anomaly in the logs
of any clice process it ran; a hang (no unit indexed for half an hour, a
request unanswered after ten minutes, a sweep out of its time); an index run
that does not finish or cannot save; a unit clice gives up as too large for
the machine; a unit clice fails or reports errors on that the compiler
compiles, or cannot check; a request either client answers with an error; a
place where the two answers differ; an error of the runner itself. A
difference a case in `tests/` pins as accepted is listed, not failed (the
framework's list, each naming its case). Counts, sizes, memory and times are
recorded beside the previous round's, never gated.

Every failure names the command that reproduces it from the repository root.

## Perf

clice under test against **clangd**, the official release of the version the
framework pins (the LLVM major xclang tracks), each in its default
configuration: clangd started from the database's directory with
`--enable-config=0`, clice with the same database named and no `clice.toml`
rule of the project's. The two servers alternate run by run on the same
machine, so a drift of the machine reaches both alike; only numbers of one
machine compare. A run that wipes a cache wipes the server's own (clice's
`.clice/`, clangd's `.cache/clangd`) and, on Linux and macOS, the OS file
cache.

| Scenario                                | What it measures                                                                                                                                                                                                                                                           |
| --------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `coldStart(file, { runs })`             | no cache: start to the file's first diagnostics, and to the first non-empty semantic tokens, hover and definition, asked at the open as an editor asks them                                                                                                                |
| `index(file, { hold })`                 | no cache, the file open: until the background index is idle; files indexed, index and cache on disk; with `hold`, what the tree holds that many seconds later                                                                                                              |
| `warmStart(file, { runs })`             | the index left: as `coldStart`, and the first references reaching a second file                                                                                                                                                                                            |
| `editLoop(file, { edits })`             | an edit at the file's end, then semantic tokens: to the new diagnostics and tokens                                                                                                                                                                                         |
| `warmRequests(file, { repeats })`       | the index loaded, each request once unmeasured, then `repeats` times: hover, definition, references, call hierarchy, workspace symbols, document symbols, semantic tokens, folding, inlay hints, completion, signature help                                                |
| `session(files, { churn })`             | the files opened in turn and edited twice each, switched through twice; unless `churn: false`, a branch switch (50 unopened sources and 10 included headers changed on disk and back) and the database rewritten unchanged, each until the servers settle; then all closed |
| `sample(files, { wipe })`               | each file in a fresh server, clangd, clice without its PCHs and clice with them (with `wipe: "cache"`, each server without any cache, then with what that left): open, semantic tokens, three edits, the whole-file requests                                               |
| `soak(file, { edits })`                 | `edits` edits, a completion and a hover every tenth: whether time and memory stay flat                                                                                                                                                                                     |
| `idle(file, { rounds, gap })`           | each request after the server sat idle for `gap`                                                                                                                                                                                                                           |
| `duringIndex(file, { every, minutes })` | edits and requests every `every` while the background index runs from scratch (at most `minutes`), then ten rounds once it is done                                                                                                                                         |
| `references(file, at)`                  | references and rename of a symbol once the index is idle                                                                                                                                                                                                                   |
| `long(files, { minutes })`              | a session cycling through the files, opening, editing, asking and closing, for `minutes`                                                                                                                                                                                   |
| `limitedIndex(file, { mb, minutes })`   | (Linux) `index` inside a memory limit of `mb`, stalled when not done within `minutes`                                                                                                                                                                                      |
| `scan({ runs })`                        | (clice) the dependency scan of the whole database, cold and warm                                                                                                                                                                                                           |

`p.pick({ seed, sources, headers, under })` lists the files a scenario takes:
sources of the database and headers they include, under the given
directories, the fixed seed picking the same ones every round.

Every run records the latencies the client observes, the memory of the
server's process tree (its peak, at the end, and at each stage of a session,
each process by role: master, each worker, clangd), its CPU seconds, and for
clice the server's own breakdown from its `[perf:*]` log lines. Answers' sizes
are recorded beside their times (items, locations, tokens), so a time is read
beside the work it bought. A perf job **fails** only when a clice run fails
(a crash, its workers' too, which its logs tell; a hang; an error answer; an
unclean exit), or when its time runs out before a scenario begins: no
scenario begins, and no wait goes on, past 320 minutes into the job. A
clangd run that fails, or the baseline's, is listed. Numbers never fail. A
baseline keeps its cache apart from clice's.

### Memory

Measured from outside, sampled every second, each process's own memory:
anonymous and shared pages on Linux, the physical footprint `top` reports on
macOS, the private commit on Windows; a peak is the highest sample, the
system's own on Windows. A process that ends between two
samples keeps the peak seen last; a watch stopped takes one more sample. Pages
mapped from files (clice's PCHs and its LMDB index, a binary its processes
share) count nowhere. The measures differ by platform: compare within one.

## Reports

A regression job's summary gives its numbers beside the previous round's,
then its new failures, each with what failed and the command reproducing it,
then the known ones with their issue, then the known issues it did not see
(fixed, or not sampled this round). A perf job's summary gives its failures
the same way, then a table per scenario: each measure's median and p90 for
every server and the ratio of clice's median to clangd's (for a scenario over
many files, the median of the files' ratios). Each job uploads its whole
results as JSON.

A job is red when it has a new failure; known failures alone leave it green.
**A round shows no new failure** when every job is green: that is what a
stable release requires of the nightly it tags.

## CI

`.github/workflows/ecosystem.yml` runs a round: every regression and perf job
of every project, each on a free runner of its platform, all at once, on the
commit of a main CI run with the `build-*` programs that run built. Once
`nightly.yml` has tagged a nightly, it dispatches the round on that commit; a
day without a new nightly has no round. A dispatch by hand names a CI run
whose artifacts still live (three days). Each job is bounded well within the
runner's six hours; on Linux it runs inside a memory scope a little smaller
than the machine, so that a clice exhausting it is killed (first) and
reported, not the runner's agent.

The round only reports: nothing waits on it but a stable release.

## Running

```
node tools/ecosystem/run.ts --jobs                              every job of a round, as JSON
node tools/ecosystem/run.ts regression <job> --dir <dir> [options]
node tools/ecosystem/run.ts perf <job> --dir <dir> [options]
node tools/ecosystem/perf/compare.ts <results.json>...           the perf tables
node tools/ecosystem/perf/logs.ts <log>... [--trace <file>]      a session's [perf:*] lines
```

`CLICE_EXECUTABLE` names the clice under test. `--dir` is where projects are
checked out and prepared (clice's own is the repository). The regression's
`--steps index|sweep`, `--file` and `--only` narrow a job; `--seed` picks
other files. Perf's `--clangd` names a clangd instead of the pinned one,
`--baseline <clice>` adds a second clice (an A/B of two commits), `--scenario`
runs some of a job's. Preparing LLVM or the kernel takes a long build and
several gigabytes of disk: a box, never this machine.

## Framework

```
tools/ecosystem/
  project.ts      project.json: configurations, parts, jobs
  prepare.ts      checkouts, steps, generated sources
  database.ts     a job's database, the headers its units include, checking commands
  script.ts       test.ts: registrations, request positions, known issues
  memory.ts       a process tree's memory and CPU
  lex.ts          a raw C/C++ lexer: where the sweep's places are
  regression/     the index, the compiler's confirmation, the sweep, the job
  perf/           the servers, a timed session, the scenarios, results and their tables, [perf:*] lines
  report.ts       the summaries
  run.ts          the command line
  **/*.test.ts    the framework's own tests, beside the code
```

`ecosystem/` is an npm workspace, so `npm run check` type-checks every
`test.ts`; `npm test -w tools` runs the framework's tests.
