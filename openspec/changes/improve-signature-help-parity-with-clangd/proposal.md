## Why

`clice` already returns basic signature help, but compared with clangd it mostly exposes raw overload candidates and misses several behaviors that make signature help reliable in real code: protocol trigger coverage, documentation, robust active-parameter mapping, and regression-tested handling for constructors, aggregates, variadics, templates, and imported declarations. Closing this gap now will make call-site assistance materially more useful and establish a spec-backed contract for one of `clice`'s core interactive features.

## What Changes

- Audit `clice`'s current signature help implementation against clangd and close the highest-value gaps that affect LSP-visible behavior.
- Improve signature construction so parameter labels, optional/defaulted arguments, variadics, templates, constructors, aggregates, and function-pointer calls are rendered consistently and the active parameter is chosen correctly.
- Extend the server and request path to advertise and honor signature-help protocol details such as trigger/retrigger characters, documentation format, and parameter label offsets where supported by the client and protocol layer.
- Add coverage for imported or module-provided declarations and other clangd-tested edge cases so signature help stays stable as the compiler pipeline evolves.

## Capabilities

### New Capabilities
- `signature-help`: Provide clangd-aligned signature help behavior for callable expressions, including robust active-parameter tracking, richer signature metadata, protocol negotiation, and regression-tested edge-case coverage.

### Modified Capabilities
- None.

## Impact

- Affected code: `src/feature/signature_help.cpp`, `src/feature/feature.h`, `src/server/master_server.cpp`, `src/server/stateless_worker.cpp`, `src/server/protocol.h`, and any eventide protocol bindings touched by signature-help capability negotiation.
- Affected tests: `tests/unit/feature/signature_help_tests.cpp`, `tests/unit/server/stateless_worker_tests.cpp`, `tests/integration/test_server.py`, and new fixtures inspired by `.llvm/clang-tools-extra/clangd/unittests/CodeCompleteTests.cpp`.
- User-visible behavior: editors should receive more complete and predictable signature help, especially while typing nested calls, constructor or aggregate initializers, variadic calls, and declarations coming from imported modules or prerequisite module builds.
