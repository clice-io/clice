# Go to Definition

Implementation: `src/server/service/lsp_client.cpp`, `src/server/worker/stateful_worker.cpp`

## Status

Go-to-definition is **partially implemented**. The index-based path queries `Indexer::query_relations()` and returns real results for indexed symbols. The in-AST fallback (stateful worker) returns an empty array.

## Implemented

- [x] Go-to-definition for indexed symbols (cross-TU, via index)
- [x] Find references (via index)

## Not Implemented

- [ ] Go-to-definition for local symbols (in-AST, stateful worker fallback)
- [ ] Go-to-declaration
- [ ] Go-to-implementation (virtual methods → overrides)

## Module Navigation

- [ ] `import module_name` → jump to module interface unit
- [ ] `import :partition` → jump to partition unit
- [ ] Module name → show all partitions (workspace symbol)

## Infrastructure

The `SemanticVisitor` has `handleModuleOccurrence()` defined but the `VISIT_DECL(ImportDecl)` is stubbed out — it returns `true` without recording anything. This needs to be implemented before module navigation can work.
