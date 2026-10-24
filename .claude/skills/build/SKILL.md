---
name: build
description: Build clice, on a box unless a local build is truly necessary. Optional args = build type (Debug | RelWithDebInfo, default RelWithDebInfo) and the caller's box id. Runs in a forked context; only the outcome returns.
context: fork
---

Build the project with the requested build type (default `RelWithDebInfo`).

## Where it runs

On a box, not on this machine: this machine hosts every agent's session, and one build saturates it. Build here only when truly necessary. The agent-box skill covers the tool itself (leases, limits, safety); for clice:

```bash
id=$(box up clice-io/clice)                     # ~1 min; only when the caller passed no id
box push $id                                    # tracked files only: `git add` new ones first
box run $id 'pixi run build RelWithDebInfo'     # run_in_background
```

The box belongs to the calling session, not to this skill: build on the id the caller passed, or lease one and report its id. Never `box down` it — the session reuses it, warm, for the tests and every later push, and an idle box stops by itself after an hour.

- A box restores the Bazel and ThinLTO caches of main's CI, RelWithDebInfo only: a Debug build there starts cold.
- A session may hold several boxes, one per kind of work — an OS or architecture, a Debug build beside the RelWithDebInfo one, the two sides of an A/B — each leased once and reused, building in parallel. Pushing never waits for a box: CI has the capacity to verify in parallel.
- Windows programs build on a linux box (the test skill runs them on a windows box).

## Commands

- Build: `pixi run build [type]` = `npx bazel build --config=[type] //:dist` (npm's bazelisk). Each type has its own output directory: `build/[type]/bin` is Bazel's output tree, with `bin/clice`, `bin/unittests` and the resource directory `lib/clang` in it — the tests and everything else run `build/[type]/bin/bin/clice`.
- Other targets or Bazel options go after `--`: `pixi run build RelWithDebInfo -- //:bin/unittests`; `//:package` and `//:symbols` are the release archive and the symbol package, `clice.gsym` (`build/[type]/bin/clice.tar.gz` and `clice-symbol.tar.xz`, `.zip` on Windows). A target of the host's OS on another architecture builds with `--platforms=@xclang//platforms:<triple>`.
- libclang is ThinLTO bitcode: a link redoes its code generation (minutes) unless lld's ThinLTO cache (`bazel/clice.bazelrc`) has it, so the first link on a machine is slow and later ones take seconds.
- `compile_commands.json` for clice's own sources: `npx bazel run @compdb//:refresh` (of `//...`).

On failure:

- Mechanical breakage (missing include, renamed symbol, stale call site after an agreed-on change): fix it, rebuild, and list every file you touched in the report.
- Design-level errors (the fix requires a decision): do not guess — report the error with `file:line` and the relevant excerpt.

Report back: build type, success or failure, files changed (if any), and for failures a digested error list — never the raw compiler spew.
