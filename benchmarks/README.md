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
   (cold start, warm start, edit loop, warm feature requests) on a real
   workspace and reports client-observed percentiles. It is server-agnostic:
   point it at clangd with `--server clangd` to A/B the same scenario.

3. **Component benchmarks** — _which design alternative is faster?_
   Standalone binaries in this directory, built with
   `-DCLICE_ENABLE_BENCHMARK=ON`, each answering one decision:
   - `scan_benchmark` — dependency-graph scan over a real CDB.
   - `pipeline_benchmark` — per-TU stage profile (preprocess with/without
     TokenBuffer, parse, index build/serialize, preamble PCH build incl.
     preamble indexing, reparse over PCH incl. interactive indexing), one
     result per file.
   - `pch_chain_benchmark` — monolithic vs chained PCH strategy (ported
     from PR #405).

## Building

```bash
cmake -B build/RelWithDebInfo -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain.cmake -DCLICE_ENABLE_BENCHMARK=ON
ninja -C build/RelWithDebInfo scan_benchmark pipeline_benchmark pch_chain_benchmark
```

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

clice's own CDB (`build/RelWithDebInfo/compile_commands.json`) doubles as
an always-available medium workload.

## Typical sessions

Stage profile of the 100 largest TUs plus a Chrome trace of one:

```bash
./build/RelWithDebInfo/bin/pipeline_benchmark --limit 100 --json /tmp/pipeline.json \
    benchmarks/workloads/llvm/build/compile_commands.json
./build/RelWithDebInfo/bin/pipeline_benchmark --filter SemaExpr.cpp --runs 3 \
    --time-trace /tmp/traces benchmarks/workloads/llvm/build/compile_commands.json
```

`--time-trace` writes clang's own `-ftime-trace` profile of the parse
stage per file — open it in [Perfetto](https://ui.perfetto.dev) to see the
frontend-internal breakdown (preprocessing, parsing, Sema, PCH
deserialization) that wall-clock stage timing cannot separate.

`--log-level info` additionally surfaces the `[perf:index_detail]` lines
from inside the index stages: semantics-table build vs projection vs
finishing within `TUIndex::build`, and the path-rekeying copy vs the
flatbuffers pack within `serialize`. The same lines appear in worker logs
of a real session, so production runs decompose identically.

E2E scenarios, clice vs clangd:

```bash
node tools/bench/bench.ts --workspace benchmarks/workloads/llvm \
    --file clang/lib/Sema/SemaExpr.cpp --json /tmp/clice.json
node tools/bench/bench.ts --workspace benchmarks/workloads/llvm \
    --file clang/lib/Sema/SemaExpr.cpp --server clangd --json /tmp/clangd.json
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
./build/RelWithDebInfo/bin/pipeline_benchmark --filter SemaExpr.cpp --runs 5 \
    benchmarks/workloads/llvm/build/compile_commands.json
clangd --check=benchmarks/workloads/llvm/clang/lib/Sema/SemaExpr.cpp \
    --compile-commands-dir=benchmarks/workloads/llvm/build 2>&1 | grep -E "preamble|AST"
```

Read them side by side as: clangd "Built preamble in N s" vs our
`pch_build`, clangd "Building AST" gap vs our `parse_pch`. Everything our
`pch_build` spends beyond clangd's preamble number is the work clice adds
to the critical path (TokenBuffer collection, preamble indexing).

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
- **One variable at a time.** The pipeline stages and the A/B knobs
  (`collect_tokens`, PCH on/off) exist so a comparison changes exactly one
  thing.

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

**Monolithic vs chained PCH** (PR #405, LLVM 21 era, 70 stdlib headers):
full chain build ~2× the monolithic build, but appending one header is
~35× faster than the monolithic full rebuild, and AST-load overhead of the
chain stays within +6% even under full deserialization.
