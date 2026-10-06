---
name: build
description: Build clice. Optional arg = build type (Debug | RelWithDebInfo, default RelWithDebInfo). Runs in a forked context — compile output and mechanical fixes stay out of the main conversation; only the outcome returns.
context: fork
---

Build the project with the requested build type (default `RelWithDebInfo`).

- Build: `pixi run build [type]` = `npx bazel build --config=[type] //:dist` (npm's bazelisk). Each type has its own output directory: `build/[type]/bin` is Bazel's output tree, with `bin/clice`, `bin/unit_tests` and the resource directory `lib/clang` in it — the tests and everything else run `build/[type]/bin/bin/clice`.
- Other targets or Bazel options go after `--`: `pixi run build RelWithDebInfo -- //:bin/scan_benchmark`; `//:package` and `//:symbols` are the release archive and the symbol package, `clice.gsym` (`build/[type]/bin/clice.tar.gz` and `clice-symbol.tar.xz`, `.zip` on Windows). A target of the host's OS on another architecture builds with `--platforms=@xclang//platforms:<triple>`.
- libclang is ThinLTO bitcode: a link redoes its code generation (minutes) unless lld's ThinLTO cache (`.bazelrc`) has it, so the first link on a machine is slow and later ones take seconds.
- `compile_commands.json` for clice's own sources: `npx bazel run @compdb//:refresh` (of `//...`; the benchmarks are `manual`: `npx bazel run @compdb//:refresh -- //... //:benchmarks`).

On failure:

- Mechanical breakage (missing include, renamed symbol, stale call site after an agreed-on change): fix it, rebuild, and list every file you touched in the report.
- Design-level errors (the fix requires a decision): do not guess — report the error with `file:line` and the relevant excerpt.

Report back: build type, success or failure, files changed (if any), and for failures a digested error list — never the raw compiler spew.
