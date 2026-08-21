# Document Links

Clickable links from source directives to their resolved target files.

> **Known limitation**: link targets are currently emitted as raw filesystem paths instead of `file:///` URIs. Clients that strictly validate DocumentUri may not navigate these links.

## Include Directives

- [x] `#include "..."` — link to resolved header file
- [x] `#include <...>` — link to resolved system header
- [x] `__has_include(...)` — link to checked file
- [x] `#embed "..."` — link to embedded resource file
- [x] `__has_embed(...)` — link to checked embed file
- [x] `#include_next` — link to the resolved next-in-search-path header
- [x] Macro-expanded include paths — resolve and link when the path is produced by a macro ([clangd#2375](https://github.com/clangd/clangd/issues/2375))

  ```cpp
  #define HEADER "config.h"
  #include HEADER  // should link to config.h
  ```

- [x] `__has_include_next(...)` — link to checked file
- [ ] Show resolved absolute path as tooltip

  ```
  #include <vector>
  // tooltip: /usr/include/c++/14/vector
  ```

## Module Declarations

- [ ] `import module_name;` — link to module interface file
- [ ] `import :partition;` — link to partition file
- [ ] `module module_name;` — link to module interface (from implementation unit)
- [ ] `export import module_name;` — link to re-exported module interface

## Changelog

| Date       | Change                                                        | PR                                                 |
| ---------- | ------------------------------------------------------------- | -------------------------------------------------- |
| 2026-04-11 | `__has_include` argument links via unified directive scanning | [#421](https://github.com/clice-io/clice/pull/421) |
| 2026-04-09 | `#embed` links; links inside the preamble preserved           | [#413](https://github.com/clice-io/clice/pull/413) |
| 2025-03-16 | `#include` directive links                                    | [#107](https://github.com/clice-io/clice/pull/107) |
