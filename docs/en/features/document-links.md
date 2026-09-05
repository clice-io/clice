# Document Links

Clickable links from source directives to their resolved target files.

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/document_links/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture doc headers and run
     `node tools/docs/feature.ts update`. -->

## Include Directives

<!-- BEGIN GENERATED ITEMS: include_directives -->

<!-- BEGIN CAPABILITY: supported -->

**Quoted includes**

`#include "..."` links to the resolved header file

Every include in the file is linked, not just the preamble run at
the top.

```snap-document_links
feature: document_links
code: |
  #include "header_a.h"
  #include "header_b.h"
  int x = 1;
  #include "header_c.h"
snapshot: |
  - { range: "7:9-7:21", target: "${WS}/header_a.h" }
  - { range: "8:9-8:21", target: "${WS}/header_b.h" }
  - { range: "10:9-10:21", target: "${WS}/header_c.h" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Angle-bracket includes**

`#include <...>` links to the header found on the search path

```snap-document_links
feature: document_links
code: |
  #include <header_a.h>
snapshot: |
  - { range: "4:9-4:21", target: "${WS}/header_a.h" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2375 -->

**Macro-expanded paths**

`#include MACRO` links the directive argument to the expanded target

```snap-document_links
feature: document_links
code: |
  #define HEADER "header_b.h"
  #include HEADER
snapshot: |
  - { range: "6:9-6:15", target: "${WS}/header_b.h" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**`#include_next` and `__has_include_next`**

Links continue down the search path

`first/wrap.h` shadows `second/wrap.h` on the search path; its
`#include_next` (guarded by `__has_include_next`) includes the second
copy. Next-in-path resolution only exists when the header is compiled
in an including TU's context — opened standalone it is compiled as its
own TU, where clang deliberately treats `#include_next` as a plain
include, so today both links land back on the first copy (as the
snapshot pins).

```snap-document_links
feature: document_links
code: |
  #include <wrap.h>

  int use_wrap = WRAP_FIRST + WRAP_SECOND;
file first/wrap.h: |
  #pragma once

  #define WRAP_FIRST 1

  #if __has_include_next(<wrap.h>)
  #include_next <wrap.h>
  #endif
file second/wrap.h: |
  #pragma once

  #define WRAP_SECOND 2
snapshot: |
  --- first/wrap.h
  - { range: "6:23-6:31", target: "${WS}/include_directives/04_include_next/first/wrap.h" }
  - { range: "7:14-7:22", target: "${WS}/include_directives/04_include_next/first/wrap.h" }

  --- main.cpp
  - { range: "13:9-13:17", target: "${WS}/include_directives/04_include_next/first/wrap.h" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`__has_include`**

The checked path links to the file it probes

```snap-document_links
feature: document_links
code: |
  #if __has_include("header_c.h")
  #include "header_c.h"
  #endif
snapshot: |
  - { range: "4:18-4:30", target: "${WS}/header_c.h" }
  - { range: "5:9-5:21", target: "${WS}/header_c.h" }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Embed Directives

<!-- BEGIN GENERATED ITEMS: embed_directives -->

<!-- BEGIN CAPABILITY: supported -->

**`#embed`**

The resource path links to the embedded file

```snap-document_links
feature: document_links
code: |
  const char data[] = {
  #embed "data.bin"
  };
snapshot: |
  - { range: "5:7-5:17", target: "${WS}/embed_directives/data.bin" }
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`__has_embed`**

The checked path links to the probed resource

```snap-document_links
feature: document_links
code: |
  #if __has_embed("data.bin")
  const char first_byte[] = {
  #embed "data.bin" limit(1)
  };
  #endif
snapshot: |
  - { range: "4:16-4:26", target: "${WS}/embed_directives/data.bin" }
  - { range: "6:7-6:17", target: "${WS}/embed_directives/data.bin" }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Presentation

<!-- BEGIN GENERATED ITEMS: presentation -->

<!-- BEGIN CAPABILITY: supported -->

**Resolved-path tooltips**

Every link carries its target's absolute path as the hover tooltip

Editors render the tooltip next to the follow-link hint, e.g.
`/usr/include/c++/14/vector (ctrl + click)`. Snapshots pin only the
link targets; the suite instead validates the tooltip against the
target on the server reply of every fixture in this corpus.

```snap-document_links
feature: document_links
code: |
  #include "header_a.h"
snapshot: |
  - { range: "9:9-9:21", target: "${WS}/header_a.h" }
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Module Declarations

<!-- BEGIN GENERATED ITEMS: module_declarations -->

<!-- BEGIN CAPABILITY: unsupported -->

**Module targets**

`import` and `module` declarations link to their interface files

```snap-document_links
feature: document_links
code: |
  export module app;

  import lib;
  import :part;
  export import lib.extra;
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
