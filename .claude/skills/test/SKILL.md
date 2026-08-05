---
name: test
description: Run clice test suites. Optional args = suite (unit | integration | smoke | snap | all, default all) and build type (Debug | RelWithDebInfo, default RelWithDebInfo). Runs in a forked context — test output stays out of the main conversation; only a digest returns.
context: fork
---

Run the requested suites (default: all) against the requested build type (default `RelWithDebInfo`).

- Unit tests: `pixi run unit-test [type]`
- Integration tests: `pixi run integration-test [type]`
- Smoke tests: `pixi run smoke-test [type]`
- Snap tests: `pixi run snap-test [type]`
- All tests: `pixi run test [type]`

Filtering specific tests:

- Unit: `pixi run unit-test [type] -- --test-filter=SuiteName.CaseName` (also `SuiteName.*`)
- Integration: `cd tests && CLICE_EXECUTABLE=../build/[type]/bin/clice npx vitest run --config integration/vitest.config.ts integration/features/some.test.ts`
- Smoke: `pixi run node tools/replay.ts tests/smoke/specific.jsonl --clice=./build/[type]/bin/clice`
- Snap: `cd tests && CLICE_EXECUTABLE=../build/[type]/bin/clice npm run snap -- -t "<feature-or-fixture>"`

Rules:

- This skill runs and reports — nothing else. Never fix code or tests from here, never skip or weaken anything, never set `UPDATE_SNAPSHOTS`. Fixing happens in the main conversation with full context.
- Never run two suites concurrently — they share `tests/data` workspaces.

Report back, per suite: pass/fail and counts. For each failure: test name, `file:line`, the assertion message or diff excerpt (trimmed), and the exact filter command to reproduce it.
