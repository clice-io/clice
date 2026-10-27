---
name: ecosystem-round
description: Read before reading a nightly ecosystem round (a red job, a perf measure past its threshold, a number that moved) — the round script, where the logs are, telling a regression from an old bug a new sample exposed, and the A/B pitfalls before calling a perf change real.
---

# Reading an ecosystem round

`ecosystem/README.md` is the contract of the jobs; this is how to read what a
round of them says.

## The round in one place

```bash
node tools/ecosystem/round.ts latest          # or a run id of ecosystem.yml
```

It writes `temp/ecosystem/<run>/`:

- `round.md` — the red jobs, each failure with whether the rounds before swept
  its file and the command reproducing it; the jobs cut short or without
  results; the
  regression numbers 1.5× the round before's; every perf measure past its
  threshold, by ratio, each with its flags. `round.json` holds the same.
- `results/ecosystem-<artifact>/` — the `results.json` and `report.md` of
  every job that uploaded them (one listed without results has none).
- `logs/ecosystem-<artifact>-logs/` — the logs of the flagged jobs. Any other
  job's: `gh run download <run> -n ecosystem-<artifact>-logs -D <dir>`.

The workflow's last job, `report`, runs the same script: the round's summary
page and its `ecosystem-round` artifact are this report. Results live 90 days,
logs 30.

A logs artifact holds, under the runner's directories:

- `_temp/projects/<project>/<configuration>/.clice/logs/<session>/` (clice's
  own project: `clice/clice/.clice/logs/`): one directory per server or
  `clice index` run, `master.log`, a log per worker (`SF-n` stateful, `SL-n`
  stateless), `index.log`. The clice under test and the baseline write into
  the same directory: a master log's first line names its version
  (`version=0.1.0+g<commit>`), and a perf run's `logs` in `results.json` names
  its sessions. A crashed process's stack is in its own log after
  `=== CRASH STACK TRACE ===` (symbolize it: the release skill).
- `_temp/logs/run.log` — what the job printed: the sweep's `sweep <file>`
  lines, a perf run's `[<scenario> <file> #<run> <server>] done`.

What a server did in the background: its `[N/M] Indexing <file>` and
`[perf:index] phase=run dispatched=` lines. Read them on both sides before
believing a memory or CPU number.

## A red job: a regression, or an old bug a new sample exposed?

The sweep samples different files every round (the seed is the date), and
each platform's database lists other units, so the same date samples other
files on each platform. The history line under each failure says:

- **newly sampled** — none of the rounds before swept the file: most likely an
  old bug. Confirm it.
- **swept before, never failing so** — a regression suspect.
- **failed so before** — an old failure no `known` entry names yet.

Confirm on a box (the agent-box skill) with two binaries: the previous nightly
(`gh release download <tag> --pattern 'clice-*.<target>.tar.gz'`, `.zip` for
Windows, 30 days) and the tested commit's (every main CI run uploads
`package-clice.<target>.tar.gz`, `.zip` for Windows, three days). Run the failure's reproduce
command with each as `CLICE_EXECUTABLE`. Failing with both: an old bug, a
board issue (write_issues) and a `known` entry in the project's `test.ts`, as
narrow as the failure. Failing only with the new one: bisect the commits
between them, with the packages of their main CI runs.

A hang that is not clice's: every log (master and workers) silent for the same
stretch, and the request answered in milliseconds once it arrived, means the
framework's own process was blocked and sent nothing. The job's `run.log` and
its `runner heap` figures show where.

## A perf measure: before calling it real

The flags in `round.md`:

- `broad` — the measure is past the threshold on most jobs at once: the
  measurement is the likelier cause.
- `<platform> only` / `every platform` — whether the job's other platforms
  agree.
- `≥3×` — more than a day's commits change: the two sides likely did
  different work.
- `negative` — the framework measured wrong.
- `one sample a side` — a time measured once per server.
- a process with two figures (`SL-0 523 vs 4 MB`) — whose memory differs.

Pitfalls of the A/B:

- **One sample.** A scenario of one run measures its start and its memory
  once per server, and one cold `initialize` varies severalfold between two
  runs of the same binary on one machine (68 and 470 ms on a macOS runner).
  Swap the binaries on the same box (`CLICE_EXECUTABLE=<old> … --baseline
<new>`) and run one binary on both sides as the negative control: a real
  change follows the binary; noise, or a bias of the order, does not.
- **Run order.** In each run the clice under test goes first, the baseline
  second. Where one server's run changes what the next one starts from (a
  file cache dropped, files a churn rewrote), the order is in the numbers.
- **The OS file cache.** Dropped before each run that wipes a cache, on Linux
  and macOS (`--drop-caches`, through sudo); Windows has no command for it, so
  its cold numbers are warm.
- **Work carried over.** The scenarios of a job share the checkout and the
  servers' caches: a session closed mid-reindex leaves the index stale, and
  every fresh server after it reindexes that unit again.
- **Runners.** Each job gets a runner of its own (EPYC 7763, 9V74, 9V45, Xeon
  8573C on Linux): only one job's numbers compare, and a regression job's
  numbers beside the round before's come from another machine, twice apart
  on the same code and sample (abseil/msvc's index, 290 and 535 s). The
  sweep's numbers depend on its sample too.

Reproduce on a box with both binaries at paths of equal length (clice finds
its resource directory beside itself, and path lengths reach some sizes), as
the row's reproduce command in the job's `report.md` does, both orders.

## Then

A real regression gets a fix with a regression test, a PR of its own (the pr
skill). An old bug gets a board issue and a `known` entry. A false alarm of
the framework gets fixed in `tools/ecosystem/`.
