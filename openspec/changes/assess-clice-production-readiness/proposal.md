## Why

`clice` has reached the stage where the core server loop starts, responds, and already exposes a meaningful set of AST-backed language features. However, it is not yet safe to treat the server as production-usable or even stable for internal dogfooding, because open-buffer semantics, compilation-context handling, stale AST/index reads, worker lifecycle, and several AST-only editor features still diverge from the behavior expected from a real C++ language server.

The project also lacks a single implementation contract that separates core server/compiler work from independently assignable feature work. A focused production-readiness change is needed now so server refactoring, feature parity work, and documentation updates can proceed in parallel without mixing concerns.

## What Changes

- Define a production-readiness plan for `clice` that separates core server/compiler obligations from AST-only feature work.
- Specify correct open-buffer versus on-disk behavior for compile, query, and navigation flows.
- Specify compilation-context and header-context behavior required for headers and multi-context source files.
- Specify the minimum AST-only feature baseline needed before internal rollout, including feature parity gaps against `clangd` where global index is not required.
- Specify server hardening requirements for worker lifecycle, stale-result rejection, observability, resource control, and error handling.
- Produce a detailed gap-analysis document under `./temp` that compares current `clice` behavior against local `clangd` sources and turns the findings into implementation workstreams.

## Capabilities

### New Capabilities
- `buffer-aware-document-model`: Define the source-of-truth rules for opened documents, unsaved edits, build generations, and request consistency.
- `compilation-context-management`: Define how `clice` chooses and applies compilation contexts for source files and headers, including header context and compilation context behavior.
- `ast-feature-baseline`: Define the AST-only feature set and quality baseline required before production dogfooding, including missing or incomplete `clangd`-style features.
- `server-production-hardening`: Define the runtime, worker, indexing, logging, and lifecycle guarantees required for reliable day-to-day use.

### Modified Capabilities

None.

## Impact

- Affected code spans `src/server`, `src/command`, `src/compile`, `src/index`, `src/feature`, and related tests under `tests/unit` and `tests/integration`.
- The change establishes the implementation contract for upcoming refactors to document state, compile scheduling, header/context selection, worker ownership/eviction, and AST-only LSP handlers.
- Documentation output will include OpenSpec artifacts under `openspec/changes/assess-clice-production-readiness/` and a detailed analysis document under `temp/`.
