Run tests. Accepts an optional argument for build type: `Debug` or `RelWithDebInfo` (default).

Available test commands:

- Unit tests: `pixi run unit-test [type]`
- Integration tests: `pixi run integration-test [type]`
- Smoke tests: `pixi run smoke-test [type]`
- All tests (unit + integration): `pixi run test [type]`

Filtering specific tests:

- Unit tests: `pixi run unit-test [type] --test-filter=SuiteName.CaseName`
- Integration tests: `pixi run pytest tests/integration -k "test_name" --executable=./build/[type]/bin/clice`
- Smoke tests: `pixi run python tests/replay.py tests/smoke/specific.jsonl --clice=./build/[type]/bin/clice`

Example usage:

- `/test` — run all tests (RelWithDebInfo)
- `/test Debug` — run all tests (Debug)
