# Document Links

Clickable links from source directives to their resolved target files.

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/document_links/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture doc headers and run
     `node tools/docs/feature.ts update`. -->

## Include Directives

<!-- BEGIN GENERATED ITEMS: include_directives -->

| Capability                               | Status    | Issues                                                      |
| ---------------------------------------- | --------- | ----------------------------------------------------------- |
| Quoted includes                          | Supported |                                                             |
| Angle-bracket includes                   | Supported |                                                             |
| Macro-expanded paths                     | Supported | [clangd#2375](https://github.com/clangd/clangd/issues/2375) |
| `#include_next` and `__has_include_next` | Partial   |                                                             |
| `__has_include`                          | Supported |                                                             |

### Quoted includes

`#include "..."` links to the resolved header file

Every include in the file is linked, not just the preamble run at
the top.

```cpp
#include "header_a.h"
#include "header_b.h"
int x = 1;
#include "header_c.h"
```

### Angle-bracket includes

`#include <...>` links to the header found on the search path

```cpp
#include <header_a.h>
```

### Macro-expanded paths

`#include MACRO` links the directive argument to the expanded target

```cpp
#define HEADER "header_b.h"
#include HEADER
```

### `#include_next` and `__has_include_next`

Links continue down the search path

`first/wrap.h` shadows `second/wrap.h` on the search path; its
`#include_next` (guarded by `__has_include_next`) includes the second
copy. Next-in-path resolution only exists when the header is compiled
in an including TU's context — opened standalone it is compiled as its
own TU, where clang deliberately treats `#include_next` as a plain
include, so today both links land back on the first copy (as the
snapshot pins).

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

### `__has_include`

The checked path links to the file it probes

```cpp
#if __has_include("header_c.h")
#include "header_c.h"
#endif
```

<!-- END GENERATED ITEMS -->

## Embed Directives

<!-- BEGIN GENERATED ITEMS: embed_directives -->

| Capability    | Status    | Issues |
| ------------- | --------- | ------ |
| `#embed`      | Supported |        |
| `__has_embed` | Supported |        |

### `#embed`

The resource path links to the embedded file

```cpp
const char data[] = {
#embed "data.bin"
};
```

### `__has_embed`

The checked path links to the probed resource

```cpp
#if __has_embed("data.bin")
const char first_byte[] = {
#embed "data.bin" limit(1)
};
#endif
```

<!-- END GENERATED ITEMS -->

## Presentation

<!-- BEGIN GENERATED ITEMS: presentation -->

| Capability             | Status    | Issues |
| ---------------------- | --------- | ------ |
| Resolved-path tooltips | Supported |        |

### Resolved-path tooltips

Every link carries its target's absolute path as the hover tooltip

Editors render the tooltip next to the follow-link hint, e.g.
`/usr/include/c++/14/vector (ctrl + click)`. Snapshots pin only the
link targets; the suite instead validates the tooltip against the
target on the server reply of every fixture in this corpus.

```cpp
#include "header_a.h"
```

<!-- END GENERATED ITEMS -->

## Module Declarations

<!-- BEGIN GENERATED ITEMS: module_declarations -->

| Capability     | Status      | Issues |
| -------------- | ----------- | ------ |
| Module targets | Unsupported |        |

### Module targets

`import` and `module` declarations link to their interface files

```cpp
export module app;

import lib;
import :part;
export import lib.extra;
```

<!-- END GENERATED ITEMS -->
