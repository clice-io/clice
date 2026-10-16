---
name: test
description: Run the clice test suites, on a box unless a local run is truly necessary; also how to test on Windows. Optional args = suite (unit | cases | all, default all) and build type (Debug | RelWithDebInfo, default RelWithDebInfo). Runs in a forked context; only a digest returns.
context: fork
---

Run the requested suites (default: all) against the requested build type (default `RelWithDebInfo`).

## Where it runs

On the box that built clice (the build skill), not on this machine unless truly necessary; the tools' own tests need no build and run here. On a box, run the suites as CI does, in the `test-run` environment: `box run $id 'pixi run -e test-run unit-test RelWithDebInfo'`.

Windows: build on a linux box, run on a windows box, as CI does. A windows box builds nothing (no Bazel cache, only `test-run`).

```bash
lin=$(box up clice-io/clice)                 # the linux box also restores CI's Windows cache
win=$(box up clice-io/clice --os windows)    # windows-arm: aarch64-w64-mingw32 below
box push $lin; box push $win
box run $lin 'pixi run build RelWithDebInfo -- --platforms=@xclang//platforms:x86_64-w64-mingw32'
box cp $lin $win build/RelWithDebInfo/bin/bin build/RelWithDebInfo/bin/lib --exclude '*.runfiles'
box run $win 'pixi run -e test-run unit-test RelWithDebInfo'
```

The copy is a few hundred MB (the debug info), a few minutes through the two tunnels.

## Commands

- Unit tests: `pixi run unit-test [type]`
- Cases: `pixi run cases-test [type]`
- Both: `pixi run test [type]`
- The tools' own tests, which need no build: `pixi run npm test -w tools`

Filtering specific tests:

- Unit: `pixi run unit-test [type] -- --test-filter=SuiteName.CaseName` (also `SuiteName.*`)
- Cases: `CLICE_CASES=<path prefix> pixi run cases-test [type]`, e.g. `CLICE_CASES=hover/` or `CLICE_CASES=hover/templates/06_template_forms`; the "Running" section of `tests/README.md` lists the rest.

The compat suite (`pixi run compat-test`) and the editors' end-to-end tests (`pixi run -e editor editor-test`) need build systems and editors installed; run them only when the caller names them.

This skill runs and reports — nothing else. Never fix code or tests from here, never set `UPDATE_SNAPSHOTS`. Fixing happens in the main conversation with full context.

Report back, per suite: pass/fail and counts. For each failure: the test name (a case's is `<topic>/<subtopic>/<case> > <test>`), the assertion message or `snap.toml` diff excerpt (trimmed), and the exact filter command to reproduce it.
