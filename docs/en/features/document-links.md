# Document Links

Clickable links from source directives to their resolved target files.

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

| Date | Change                                                      | PR  |
| ---- | ----------------------------------------------------------- | --- |
| —    | Include directive links (#include, \_\_has_include, #embed) | —   |
