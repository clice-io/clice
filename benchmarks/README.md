# Benchmarks

Performance work on clice runs on three layers. Pick the layer that answers
your question:

1. **Instrumentation** — _where does the time go in a real run?_ Every
   server run emits `[perf:<topic>] key=value` log lines (see
   `src/support/logging.h`) covering startup phases, per-file compiles,
   PCH/PCM/index builds, cache hits, request latencies and index queries.
   `tools/bench/perf_report.ts` aggregates any log into per-series
   percentiles and can export a Chrome trace. This layer needs no special
   build and works on logs users attach to issue reports.

2. **Scenario harness** — _what does the user experience end to end?_
   `tools/bench/bench.ts` drives a server over LSP through fixed scenarios
   (cold start, full background index, restart on the index, edit loop,
   warm feature requests) on a real workspace and reports client-observed
   latencies, the memory of the server's processes and the index's size.
   It is server-agnostic: given `--server clice --server clangd` it
   alternates the two run by run on the same files, and
   `tools/bench/compare.ts` tabulates the medians side by side.

3. **Component benchmarks** — _which design alternative is faster?_
   Standalone binaries in this directory, Bazel targets of their own
   (`//:benchmarks`), each answering one decision:
   - `pipeline_benchmark` — per-TU stage profile (preprocess, parse, index
     build/serialize, preamble PCH build incl. preamble indexing, reparse over
     PCH incl. interactive indexing), one result per file.

## Building

```bash
pixi run build RelWithDebInfo -- //:benchmarks
```

`//:benchmarks` builds every one; name single ones (`//:bin/pipeline_benchmark`)
to build less. They land in `build/RelWithDebInfo/bin/bin`.

Always benchmark `RelWithDebInfo`; Debug numbers are meaningless.

## Workloads

Benchmarks against real projects must be pinned to be comparable.
`workloads.json` records project + ref + configure command;
`fetch_workload.py <name>` materializes one under `benchmarks/workloads/`
(shallow clone + CMake configure — no build needed, only the
compile_commands.json):

```bash
python benchmarks/fetch_workload.py llvm
```

clice's own CDB (`npx bazel run @compdb//:refresh` writes `compile_commands.json`)
doubles as an always-available medium workload.

## Typical sessions

The dependency-graph scan, cold and warm, over a workload's database — the
CLI times it itself:

```bash
./build/RelWithDebInfo/bin/bin/clice scan --workspace benchmarks/workloads/llvm/build --runs 20
```

Stage profile of the 100 largest TUs plus a Chrome trace of one:

```bash
./build/RelWithDebInfo/bin/bin/pipeline_benchmark --limit 100 --json /tmp/pipeline.json \
    benchmarks/workloads/llvm/build/compile_commands.json
./build/RelWithDebInfo/bin/bin/pipeline_benchmark --filter SemaExpr.cpp --runs 3 \
    --time-trace /tmp/traces benchmarks/workloads/llvm/build/compile_commands.json
```

`--time-trace` writes clang's own `-ftime-trace` profile of the parse
stage per file — open it in [Perfetto](https://ui.perfetto.dev) to see the
frontend-internal breakdown (preprocessing, parsing, Sema, PCH
deserialization) that wall-clock stage timing cannot separate.

`--log-level info` additionally surfaces the `[perf:index_detail]` lines
from inside the index stage: semantics-table build vs projection vs
finishing vs per-file blob encoding vs the envelope pack within
`build_tu_index`. The same lines appear in worker logs of a real session,
so production runs decompose identically.

E2E scenarios, clice vs clangd:

```bash
node tools/bench/bench.ts --workspace benchmarks/workloads/llvm \
    --file clang/lib/Sema/SemaExpr.cpp --server clice --server clangd \
    --scenario cold_start --runs 5 --json /tmp/semaexpr.json
node tools/bench/compare.ts /tmp/semaexpr.json
```

Breakdown of a real (non-benchmark) session from its logs:

```bash
node tools/bench/perf_report.ts <logging_dir>/<session>/*.log --trace /tmp/trace.json
```

Single-file stage comparison against clangd — pair `pipeline_benchmark`
(one file selected via `--filter`) with `clangd --check`, which prints its
preamble build and AST build times for the same TU without a server or
background indexing in the way:

```bash
./build/RelWithDebInfo/bin/bin/pipeline_benchmark --filter SemaExpr.cpp --runs 5 \
    benchmarks/workloads/llvm/build/compile_commands.json
clangd --check=benchmarks/workloads/llvm/clang/lib/Sema/SemaExpr.cpp \
    --compile-commands-dir=benchmarks/workloads/llvm/build 2>&1 | grep -E "preamble|AST"
```

Read them side by side as: clangd "Built preamble in N s" vs our
`pch_build`, clangd "Building AST" gap vs our `parse_pch`. Everything our
`pch_build` spends beyond clangd's preamble number is the work clice adds
to the critical path (preamble indexing).

## Method rules

- **Fix the machine, compare on the machine.** Absolute numbers are not
  comparable across hosts; run both sides of any A/B on the same machine in
  the same session.
- **Cold vs warm is a protocol, not an accident.** The harness's
  `cold_start` wipes the cache dirs; everything else is warm. For component
  benchmarks the first run warms the OS file cache — use `--runs` and look
  at percentiles, not single samples.
- **Idle machine.** No concurrent builds. On WSL2 specifically, do not run
  ninja alongside a benchmark: page-cache-sensitive numbers wobble because
  WSL2 reclaims mmap'd cache aggressively.
- **One variable at a time.** The pipeline stages and the PCH on/off knob
  exist so a comparison changes exactly one thing.

## Reference numbers

Indicative magnitudes from past measured runs — **not** authoritative
baselines (single machine, dated). Re-measure locally before drawing
conclusions.

**Interactive path** (WSL2, RelWithDebInfo, 2026-07; probe TU ≈ 150k
semantic nodes over `<iostream>/<vector>/<string>`):

| shape                                          | time                             |
| ---------------------------------------------- | -------------------------------- |
| parse, no PCH                                  | ~330 ms                          |
| parse over warm preamble PCH (didChange path)  | ~50 ms                           |
| TUIndex build, interactive (`interested_only`) | ~0.1–1.4 ms                      |
| warm hover end-to-end (tiny project)           | ~0.9 ms (feature itself ~0.2 ms) |

The didChange experience is parse-dominated; features and index are
sub-millisecond next to it.
