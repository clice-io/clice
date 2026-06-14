# Code Completion

Implementation: `src/feature/code_completion.cpp`, `src/syntax/completion.cpp`, `src/server/compiler/compiler.cpp`

## Include Path Completion

Triggered by `<`, `"`, `/` characters. Handled before AST (preamble-level, no compilation needed).

- [x] `#include <` — system/angled include paths
- [x] `#include "` — quoted include paths (project-local first)
- [x] Subdirectory traversal (`sys/`) — appends `/` to directory candidates
- [x] Respects compiler search paths from compilation database
- [ ] `#include_next` context detection
- [ ] Filter out already-included headers

## Module Import Completion

Detected via text context analysis when cursor is after `import`/`export import`. Handled before AST (preamble-level).

- [x] `import ` — all known module names [^clangd-2622]
- [x] `export import ` — same as above
- [x] Prefix filtering (typing narrows results)
- [x] Auto-append `;` via `insert_text`
- [x] `CompletionItemKind::Module` icon [^clangd-2623]
- [ ] Exclude self-module from results (self-import invalid) — **FIXME**
- [ ] `import :` — partition completion (only current module's partitions) [^clangd-2622]
- [ ] `import foo:` — specific module's partitions
- [ ] Hierarchical dot-completion (`import std.` → show `io`, `core`)
- [ ] Filter out other module's private partitions [^clangd-2622]
- [ ] Trigger on space character (VS Code extension middleware) [^pr-460]

## Semantic Code Completion

Triggered by `.`, `->`, `::`, or quickSuggestions. Forwarded to Clang `CodeCompleteConsumer` via stateless worker.

### Member Access
- [x] `.` — struct/class members
- [x] `->` — pointer member access (with fixup)
- [x] `::` — namespace/class scope members

### Symbols
- [x] Unqualified name lookup (local vars, functions, types)
- [x] Qualified name lookup (`std::`)
- [x] Argument-dependent lookup (ADL) candidates
- [x] Keyword completion (if, for, while, etc.)
- [x] Macro completion
- [x] Snippet patterns (function bodies, control flow)

### Functions
- [x] Function overload grouping (`bundle_overloads` option)
- [x] Parameter placeholder snippets (`enable_function_arguments_snippet`)
- [ ] Template argument placeholders (`enable_template_arguments_snippet`)
- [ ] Auto-insert parentheses (`insert_paren_in_function_call`)

### Filtering & Ranking
- [x] Fuzzy matching with scoring
- [x] Prefix-based filtering
- [ ] Result limit (`CodeCompletionOptions.limit`)
- [x] Filter out recovery context results (`CCC_Recovery`)
- [ ] Frecency/recently-used boosting
- [x] Filter out `_`-prefixed internal symbols (unless user typed `_`)

## Trigger Characters

Registered: `. < > : " / *` (space is NOT registered server-side — requires extension middleware)

| Character | Context | Behavior |
|-----------|---------|----------|
| `.` | Member access | Semantic completion |
| `->` | Pointer member | Via `.` trigger + Clang fixup |
| `::` | Via `:` trigger | Scope completion |
| `<` | `#include <` | Include path completion |
| `>` | Template close | Semantic completion |
| `"` | `#include "` | Include path completion |
| `/` | Path separator | Include path continuation |
| `*` | Pointer deref | Semantic completion |
| ` ` | After `import` | Module name completion (extension-gated) |

## Changelog

| Date | Change | PR |
|------|--------|-----|
| — | Initial include/semantic completion | — |
| — | Module import completion (flat prefix) | — |
| 2025-06 | Space trigger + extension middleware gate | [#460](https://github.com/clice-project/clice/pull/460) |

[^clangd-2622]: [clangd#2622](https://github.com/clangd/clangd/issues/2622) — Remove other modules' partitions from suggestions `open`
[^clangd-2623]: [clangd#2623](https://github.com/clangd/clangd/issues/2623) — Fix missing icons for module import suggestions `closed`
[^clangd-2626]: [clangd#2626](https://github.com/clangd/clangd/issues/2626) — Single-token completion for `import`/`module` keywords `open`
[^clangd-2577]: [clangd#2577](https://github.com/clangd/clangd/issues/2577) — Auto-insert `import` statement (like auto-include) `open`
[^vscode-67714]: [vscode#67714](https://github.com/microsoft/vscode/issues/67714) — Space as trigger character discussion `closed`
[^pr-460]: [#460](https://github.com/clice-project/clice/pull/460) — Space trigger + extension middleware gate
