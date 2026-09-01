# Document Links

Clickable links from source directives to their resolved target files.

<!-- The checklist sections below are generated from the snapshot fixtures in
     tests/snap/document_links/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture doc headers and run
     `node tools/docs/feature.ts update`. -->

## Include Directives

<!-- BEGIN GENERATED ITEMS: Include Directives -->

- [x] Quoted includes — `#include "..."` links to the resolved header file

  Every include in the file is linked, not just the preamble run at
  the top.

  <details>
  <summary>Example</summary>

  ```cpp
  #include "header_a.h"
  #include "header_b.h"
  int x = 1;
  #include "header_c.h"
  ```

  </details>

- [x] Angle-bracket includes — `#include <...>` links to the header found on the search path

  <details>
  <summary>Example</summary>

  ```cpp
  #include <header_a.h>
  ```

  </details>

- [x] Macro-expanded paths — `#include MACRO` links the directive argument to the expanded target ([clangd#2375](https://github.com/clangd/clangd/issues/2375))

  <details>
  <summary>Example</summary>

  ```cpp
  #define HEADER "header_b.h"
  #include HEADER
  ```

  </details>

- [ ] `#include_next` and `__has_include_next` — links continue down the search path _(partial)_

  `first/wrap.h` shadows `second/wrap.h` on the search path; its
  `#include_next` (guarded by `__has_include_next`) includes the second
  copy. Next-in-path resolution only exists when the header is compiled
  in an including TU's context — opened standalone it is compiled as its
  own TU, where clang deliberately treats `#include_next` as a plain
  include, so today both links land back on the first copy (as the
  snapshot pins).

  <details>
  <summary>Example</summary>

  `main.cpp`:

  ```cpp
  #include <wrap.h>

  int use_wrap = WRAP_FIRST + WRAP_SECOND;
  ```

  `first/wrap.h`:

  ```cpp
  #pragma once

  #define WRAP_FIRST 1

  #if __has_include_next(<wrap.h>)
  #include_next <wrap.h>
  #endif
  ```

  `second/wrap.h`:

  ```cpp
  #pragma once

  #define WRAP_SECOND 2
  ```

  </details>

- [x] `__has_include` — the checked path links to the file it probes

  <details>
  <summary>Example</summary>

  ```cpp
  #if __has_include("header_c.h")
  #include "header_c.h"
  #endif
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Embed Directives

<!-- BEGIN GENERATED ITEMS: Embed Directives -->

- [x] `#embed` — the resource path links to the embedded file

  <details>
  <summary>Example</summary>

  ```cpp
  const char data[] = {
  #embed "data.bin"
  };
  ```

  </details>

- [x] `__has_embed` — the checked path links to the probed resource

  <details>
  <summary>Example</summary>

  ```cpp
  #if __has_embed("data.bin")
  const char first_byte[] = {
  #embed "data.bin" limit(1)
  };
  #endif
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Presentation

<!-- BEGIN GENERATED ITEMS: Presentation -->

- [x] Resolved-path tooltips — every link carries its target's absolute path as the hover tooltip

  Editors render the tooltip next to the follow-link hint, e.g.
  `/usr/include/c++/14/vector (ctrl + click)`. Snapshots pin only the
  link targets; the suite instead validates the tooltip against the
  target on the server reply of every fixture in this corpus.

  <details>
  <summary>Example</summary>

  ```cpp
  #include "header_a.h"
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Module Declarations

<!-- BEGIN GENERATED ITEMS: Module Declarations -->

- [ ] Module targets — `import` and `module` declarations link to their interface files

  <details>
  <summary>Example</summary>

  ```cpp
  export module app;

  import lib;
  import :part;
  export import lib.extra;
  ```

  </details>

<!-- END GENERATED ITEMS -->
