# Document Links

Implementation: `src/feature/document_links.cpp`

## Include Directives

- [x] `#include "..."` — link to resolved header file
- [x] `#include <...>` — link to resolved system header
- [x] `__has_include(...)` — link to checked file
- [x] `#embed "..."` — link to embedded resource file
- [x] `__has_embed(...)` — link to checked embed file

## Module Declarations

- [ ] `import module_name;` — link to module interface file [^clangd-2622]
- [ ] `import :partition;` — link to partition file [^clangd-2622]
- [ ] `module module_name;` — link to module interface (from implementation unit)

## Implementation Notes

Links are computed by the stateful worker via `feature::document_links()` using `CompilationUnitRef::directives()`, which tracks includes, has_includes, embeds, and has_embeds per file. A PCH-cached fallback (`document_links_json`) is used when no worker is available.

[^clangd-2622]: [clangd#2622](https://github.com/clangd/clangd/issues/2622) — Module partition support tracking `open`
