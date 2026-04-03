## 1. Buffer-Aware Document Model

- [ ] 1.1 Introduce an explicit opened-document state model that tracks text, editor version, build generation, AST generation, and readiness state in the master/worker boundary
- [ ] 1.2 Make `didOpen`, `didChange`, `didSave`, and `didClose` update one consistent source-of-truth flow for opened documents
- [ ] 1.3 Replace the `ensure_compiled()` stub with generation-aware gating for AST-backed requests
- [ ] 1.4 Reject or defer stale AST-backed feature requests instead of serving results from older generations
- [ ] 1.5 Prevent AST-backed feature execution on fatal or otherwise unusable compilation units
- [ ] 1.6 Add tests that cover unsaved edits, stale compile completion discard, and close-time state release

## 2. Compilation Context Management

- [ ] 2.1 Implement real project-derived compilation-context selection for headers without direct compile commands
- [ ] 2.2 Define and persist distinct runtime identities for header contexts and source-file compilation contexts
- [ ] 2.3 Make query paths accept an explicit active context instead of blindly unioning context-sensitive results
- [ ] 2.4 Surface missing or degraded compilation-context selection through logs or diagnostics
- [ ] 2.5 Add tests for header opening, project-context borrowing, and multi-context file handling

## 3. Server Runtime Hardening

- [ ] 3.1 Wire `didClose` to worker eviction and ownership cleanup so closed documents do not linger in worker memory
- [ ] 3.2 Surface worker IPC failures, crashes, and timeouts as explicit server-side errors with useful logs
- [ ] 3.3 Enforce configured resource controls such as worker memory limits instead of relying on hardcoded thresholds
- [ ] 3.4 Prioritize opened-document compile work ahead of non-urgent background indexing
- [ ] 3.5 Expand runtime logging for compile commands, context selection, worker failure causes, and request/result mismatches
- [ ] 3.6 Add lifecycle and scheduling tests that cover worker cleanup, timeout handling, and active-edit priority

## 4. Live-State Navigation Correctness

- [ ] 4.1 Resolve source-side symbol lookup for opened files from the current AST before consulting disk-backed index data
- [ ] 4.2 Define how active-document state is combined with index-backed remote results for definition and related navigation flows
- [ ] 4.3 Remove or disable stubbed navigation fallback paths that cannot yet return correct results
- [ ] 4.4 Add tests for navigation from unsaved buffers and mixed AST/index result paths

## 5. AST-Only Feature Baseline

- [ ] 5.1 Enrich hover so it returns semantic detail beyond bare symbol kind and name
- [ ] 5.2 Add `documentHighlight` as an AST-backed feature with end-to-end capability advertisement and routing
- [ ] 5.3 Expose `selectionRange` using the existing local selection machinery
- [ ] 5.4 Upgrade completion and signature help with parameter-aware insertion and richer callable assistance
- [ ] 5.5 Respect requested scope for inlay-hint computation instead of forcing full-document evaluation
- [ ] 5.6 Implement the first production-worthy AST-only code actions or stop advertising stubbed code-action support
- [ ] 5.7 Audit advertised AST-only capabilities and remove any remaining stub or placeholder surfaces
- [ ] 5.8 Add or update feature-level tests and golden cases for every newly exposed AST-only feature

## 6. Documentation And Readiness Tracking

- [ ] 6.1 Write and maintain the detailed production-readiness gap analysis in `temp/`
- [ ] 6.2 Update developer documentation for opened-buffer semantics, compilation-context handling, and feature ownership boundaries
- [ ] 6.3 Define a dogfooding readiness checklist that combines server/core acceptance criteria with the minimum AST-only feature baseline
