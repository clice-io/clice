# Modulize

## Overview

`clice modulize` turns the third-party libraries of a program into C++20 named modules without touching a source file. Each library's headers stay the source of truth: a generated module interface unit includes them in its global module fragment and exports every namespace-scope name they declare. The program keeps its `#include` directives; a directory first on its include path turns the wrapped headers into empty files, and a prelude it force-includes imports the modules and replays the macros their headers defined.

**Usage**: `clice modulize --partition <file> --out <dir> [--std <dir>] [--scope <glob,...>] [--workspace <dir>] [--configuration <tag>]`

Every fact comes from the persisted index, as [`clice analyze modules --view interface`](./analyze.md#modules) reports it: run `clice index` first. The command writes files and prints what the build needs; it never edits a build file.

## Partition

The partition file groups files into modules by globs over workspace-relative paths (absolute ones outside the workspace), first match wins. Files no glob claims are the program's and stay headers.

```json
{
  "modules": [
    { "name": "libc", "files": ["/opt/llvm/include/c++/v1/*.h"] },
    { "name": "std", "files": ["/opt/llvm/include/c++/v1/**"], "external": true },
    {
      "name": "libc",
      "files": ["/usr/include/**", "/opt/llvm/lib/clang/23/include/**"],
      "textual": true,
      "provides": "std.compat"
    },
    { "name": "llvm", "files": ["third_party/llvm/include/**"] },
    { "name": "fmt", "files": ["third_party/fmt/include/**"] }
  ]
}
```

- A module with no flag is wrapped: modulize writes its interface unit.
- `"external": true` marks a module an existing interface stands for. Only `std` is supported: with `--std` naming libc++'s module sources (`share/libc++/v1`), it is imported as `std.compat`, and the standard headers `std.cppm` includes are emptied.
- `"textual": true` keeps a module headers, as the C library and the compiler's own headers stay beside `import std`. What the program reaches of them only through emptied headers is included or replayed in the prelude.
- `"provides": "std.compat"` names the module exporting a textual module's names, so only what it leaves out needs a real header.

One module per library and one module for all of them are both a partition away; the scope has to take in every file the partition names.

## Output

Under `--out`:

- `<module>.cppm` per wrapped module, and `<module>.macros.h` with the macros its importers need, in definition order.
- `mirror/<module>/`: an empty file for each header files of other modules include, by the name they include it with; `mirror/std/` for the standard headers.
- `prelude.h`: the C library headers still needed, `import std.compat;`, every import, every macro header.

Files whose content did not change keep their timestamps. The files a previous run wrote that this one no longer produces are removed; `--out/.modulize` lists what a run wrote, and nothing else under `--out` is touched.

On stdout, the build plan: module sources, mirrors and the prelude relative to `--out`, libc++'s sources and each library's include roots as found:

```json
{
  "stdSources": ["share/libc++/v1/std.cppm", "share/libc++/v1/std.compat.cppm"],
  "modules": [
    {
      "name": "llvm",
      "source": "llvm.cppm",
      "imports": [],
      "includeRoots": ["third_party/llvm/include"],
      "mirrors": ["mirror/std"]
    }
  ],
  "mirrors": ["mirror/std", "mirror/llvm"],
  "prelude": "prelude.h",
  "warnings": []
}
```

The build compiles `stdSources` and each module in order, each with its library's include roots and its `mirrors` first on the include path, and adds `mirrors` and `-include prelude.h` to every program compilation.

## Known Limitations

- Module units must be built with full BMIs (`-fno-modules-reduced-bmi` on clang): a reduced BMI drops the global module fragment's declarations the purview never names, partial specializations among them.
- A header holding an internal-linkage entity the program names stays textual: no module can export it. The prelude does not include such a header; a program file that reached it only through the library's emptied headers includes it itself.
- A header other files include by a name relative to their own directory (`"../foo.h"`) cannot be emptied; modulize warns about it.
- Every translation unit using a wrapped library has to see it through the module; mixing textual includes of the same headers with the import breaks on redeclarations clang cannot merge.
- Every program compilation imports every module and replays every macro header, whichever libraries it included before.
- The standard library module is libc++'s.
