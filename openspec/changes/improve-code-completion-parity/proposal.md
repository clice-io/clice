## Why

`clice` already answers `textDocument/completion`, but the current implementation is still a thin Sema wrapper: it gathers only compiler candidates, applies local fuzzy filtering, and returns minimal `CompletionItem` fields. Compared with the local `clangd` implementation under `.llvm/clang-tools-extra/clangd`, it is missing most of the completion pipeline that makes results accurate, stable, and editor-friendly: client capability negotiation, context-aware triggering, richer ranking, index-backed augmentation, richer rendering, and completion-related edits.

This is worth addressing now because `clice` already has the infrastructure that can support a stronger design: background project indexing, merged per-file index shards, header-context modeling, stateless completion workers, and PCH/PCM plumbing for module-aware compilation. Closing the clangd gap will materially improve day-to-day usability, while `clice`'s existing header-context, module, and instantiation-aware architecture creates opportunities to implement completion behaviors clangd does not currently provide.

## What Changes

- Define a full code-completion pipeline for `clice` instead of the current direct Sema-to-LSP conversion path.
- Add parity-oriented completion behavior for the highest-value clangd features:
  - client capability negotiation for snippets, documentation format, label details, completion item kinds, and overload bundling
  - trigger-character handling and suppression of obviously spurious auto-triggered completion
  - richer `CompletionItem` rendering including `filterText`, stable `sortText`, signatures/details, return types, documentation, deprecation, snippet suffixes, and completion-related edits
  - result limiting and `isIncomplete` signaling
  - completion candidates from project index and local identifiers in addition to Sema results
  - ranking that combines fuzzy match with semantic/contextual signals rather than raw name matching alone
- Define how `clice` should attach edits associated with completion candidates, including fix-its and insertion of missing dependencies when the completion source can justify them.
- Specify `clice`-specific completion extensions that go beyond clangd's current model:
  - header-context-aware completion for headers shared by multiple includers
  - module-aware completion that exploits existing PCM dependency tracking
  - instantiation-aware ranking and filtering for template-heavy code
- Add targeted unit and integration coverage so completion behavior can evolve without regressions.

## Capabilities

### New Capabilities
- `code-completion`: Provide context-aware, multi-source C++ code completion with rich LSP rendering, ranking, dependency edits, and `clice`-specific support for header contexts, modules, and template instantiation.

### Modified Capabilities
- None.

## Impact

- Affected code: `src/feature/code_completion.cpp`, `src/feature/feature.h`, `src/server/master_server.cpp`, `src/server/stateless_worker.cpp`, `src/server/protocol.h`, and completion-related protocol wiring in the IPC/LSP layer.
- Likely affected subsystems: project/merged index integration, document/client capability tracking, completion request routing, and any helper code used for include/import insertion or completion scoring.
- Affected tests: completion unit tests, stateless worker tests, server integration tests, module fixtures, and new comparison-oriented cases for header-context and template-heavy completion.
