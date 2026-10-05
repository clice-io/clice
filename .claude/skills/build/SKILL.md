---
name: build
description: Build clice. Optional arg = build type (Debug | RelWithDebInfo, default RelWithDebInfo). Runs in a forked context — compile output and mechanical fixes stay out of the main conversation; only the outcome returns.
context: fork
---

Build the project with the requested build type (default `RelWithDebInfo`).

- Build: `pixi run build [type]` — Bazel (npm's bazelisk, `npx bazel`) builds `//:dist` with `--config=[type]`, and `scripts/build.py` lays it out in `build/[type]/{bin,lib}`, where the tests and everything else run it.
- Other targets or Bazel options go after `--`: `pixi run build RelWithDebInfo -- //:scan_benchmark //:resource_dir`, or `-- --config=release` to link as releases ship clice (libclang's ThinLTO bitcode; minutes per link).
- Bazel directly, without the layout: `npx bazel build --config=[type] //:clice` (targets: `clice`, `unit_tests`, the benchmarks; `BUILD.bazel`).
- `compile_commands.json` for clice's own sources: `pixi run compile-commands [type]`.

On failure:

- Mechanical breakage (missing include, renamed symbol, stale call site after an agreed-on change): fix it, rebuild, and list every file you touched in the report.
- Design-level errors (the fix requires a decision): do not guess — report the error with `file:line` and the relevant excerpt.

Report back: build type, success or failure, files changed (if any), and for failures a digested error list — never the raw compiler spew.
