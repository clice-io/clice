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

```snap
tests/snap/document_links/include_directives/01_quoted_include.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Angle-bracket includes**

`#include <...>` links to the header found on the search path

```snap
tests/snap/document_links/include_directives/02_angle_include.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2375 -->

**Macro-expanded paths**

`#include MACRO` links the directive argument to the expanded target

```snap
tests/snap/document_links/include_directives/03_macro_include.cpp
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
include, so today both links land back on the first copy.

```snap
tests/snap/document_links/include_directives/04_include_next/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`__has_include`**

The checked path links to the file it probes

```snap
tests/snap/document_links/include_directives/05_has_include.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Embed Directives

<!-- BEGIN GENERATED ITEMS: embed_directives -->

<!-- BEGIN CAPABILITY: supported -->

**`#embed`**

The resource path links to the embedded file

```snap
tests/snap/document_links/embed_directives/01_embed.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`__has_embed`**

The checked path links to the probed resource

```snap
tests/snap/document_links/embed_directives/02_has_embed.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Presentation

<!-- BEGIN GENERATED ITEMS: presentation -->

<!-- BEGIN CAPABILITY: supported -->

**Resolved-path tooltips**

Every link carries its target's absolute path as the hover tooltip

Editors render the tooltip next to the follow-link hint, e.g.
`/usr/include/c++/14/vector (ctrl + click)`.

```snap
tests/snap/document_links/presentation/01_tooltip.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Module Declarations

<!-- BEGIN GENERATED ITEMS: module_declarations -->

<!-- BEGIN CAPABILITY: unsupported -->

**Module targets**

`import` and `module` declarations link to their interface files

```snap
tests/snap/document_links/module_declarations/01_modules.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
