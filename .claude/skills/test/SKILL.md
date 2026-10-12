---
name: test
description: Run clice test suites. Optional args = suite (unit | cases | all, default all) and build type (Debug | RelWithDebInfo, default RelWithDebInfo). Runs in a forked context — test output stays out of the main conversation; only a digest returns.
context: fork
---

Run the requested suites (default: all) against the requested build type (default `RelWithDebInfo`).

- Unit tests: `pixi run unit-test [type]`
- Cases: `pixi run cases-test [type]`
- Both: `pixi run test [type]`
- The tools' own tests, which need no build: `pixi run npm test -w tools`

Filtering specific tests:

- Unit: `pixi run unit-test [type] -- --test-filter=SuiteName.CaseName` (also `SuiteName.*`)
- Cases: `CLICE_CASES=<path prefix> CLICE_EXECUTABLE=$PWD/build/[type]/bin/bin/clice pixi run npm run cases -w tests`, e.g. `CLICE_CASES=hover/` or `CLICE_CASES=hover/templates/06_template_forms`; the "Running" section of `tests/README.md` lists the rest.

The compat suite (`pixi run compat-test`) and the editors' end-to-end tests (`pixi run -e editor editor-test`) need build systems and editors installed; run them only when asked.

Rules:

- This skill runs and reports — nothing else. Never fix code or tests from here, never skip or weaken anything, never set `UPDATE_SNAPSHOTS`. Fixing happens in the main conversation with full context.

Report back, per suite: pass/fail and counts. For each failure: the test name (a case's is `<topic>/<subtopic>/<case> > <test>`), the assertion message or `snap.toml` diff excerpt (trimmed), and the exact filter command to reproduce it.
