## 1. Completion Request Contract

- [ ] 1.1 Audit the current `textDocument/completion` request path and extend the master/worker protocol to carry trigger context, requested limit, and normalized client completion capabilities.
- [ ] 1.2 Store completion-related client capabilities from `initialize` in the server and thread them into completion requests instead of relying on default `CodeCompletionOptions`.
- [ ] 1.3 Change completion responses from a raw item vector to a completion-list shape that can represent `isIncomplete` and future completion metadata cleanly.

## 2. Internal Completion Pipeline

- [ ] 2.1 Introduce an internal completion-candidate/result model that separates collection, deduplication/bundling, scoring, truncation, and final LSP rendering.
- [ ] 2.2 Refactor `src/feature/code_completion.cpp` so Sema collection populates the internal model instead of directly constructing final `CompletionItem`s.
- [ ] 2.3 Implement capability-aware rendering for snippets, signatures/details, documentation, deprecation, `filterText`, and stable `sortText`.
- [ ] 2.4 Add request-side suppression for obviously spurious auto-triggered completions and preserve correct fallback behavior for manual completion.

## 3. Candidate Sources And Ranking

- [ ] 3.1 Add local-identifier collection from the active buffer/file context and merge those candidates with Sema results.
- [ ] 3.2 Implement semantic deduplication and overload bundling based on insertion semantics rather than label-only equality.
- [ ] 3.3 Replace raw fuzzy-only ordering with explicit heuristic scoring that incorporates scope visibility, expected type, deprecation, and active compilation-context signals.
- [ ] 3.4 Enforce result limits after scoring and verify that truncation drives `isIncomplete` correctly.

## 4. Index Completion And Completion Edits

- [ ] 4.1 Extend completion-relevant index data structures and serialization to store the metadata needed for index-backed completion rendering and ranking.
- [ ] 4.2 Add project-index candidate lookup for completion and merge index candidates with Sema/identifier candidates without duplicating visible entries.
- [ ] 4.3 Preserve and render Sema fix-it edits as completion-associated edits.
- [ ] 4.4 Implement dependency insertion edits for header-backed symbols once index metadata can identify the correct declaration owner and insertion path.

## 5. `clice`-Specific Completion Extensions

- [ ] 5.1 Bind completion in headers to the active header-context selection model instead of treating shared headers as context-free.
- [ ] 5.2 Integrate available PCM/module dependency information into candidate visibility and ranking for module-aware completion.
- [ ] 5.3 Implement module-aware dependency insertion policy for completion items that should add `import`-style dependencies instead of header includes.
- [ ] 5.4 Add template-instantiation-aware ranking signals where `clice` can derive concrete instantiation or expected-type compatibility.

## 6. Validation

- [ ] 6.1 Add focused unit tests for completion rendering, capability negotiation, overload bundling, sorting/filtering metadata, and `isIncomplete` behavior.
- [ ] 6.2 Add unit or fixture-based tests for identifier augmentation, index-backed completion, fix-it edits, and dependency insertion edits.
- [ ] 6.3 Add integration coverage for shared-header contexts, module-aware completion, and template-heavy completion cases.
- [ ] 6.4 Run the relevant completion, worker, index, and integration test targets and fix regressions uncovered during verification.
