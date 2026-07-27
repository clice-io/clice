Run tests. Accepts an optional argument for build type: `Debug` or `RelWithDebInfo` (default).

Available test commands:

- Unit tests: `pixi run unit-test [type]`
- Integration tests: `pixi run integration-test [type]`
- Smoke tests: `pixi run smoke-test [type]`
- Snap tests: `pixi run snap-test [type]`
- All tests: `pixi run test [type]`

Filtering specific tests:

- Unit tests: `pixi run unit-test [type] --test-filter=SuiteName.CaseName`
- Integration tests: `cd tests && CLICE_EXECUTABLE=../build/[type]/bin/clice npx vitest run integration/features/some.test.ts`
- Smoke tests: `pixi run node tools/replay.ts tests/smoke/specific.jsonl --clice=./build/[type]/bin/clice`
- Snap tests: `node tools/snap.ts --clice=./build/[type]/bin/clice` (add `--update` to accept changes; run before integration when updating shared snapshots)

Example usage:

- `/test` — run all tests (RelWithDebInfo)
- `/test Debug` — run all tests (Debug)
