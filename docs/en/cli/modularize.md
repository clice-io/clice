# Modularize

## Overview

`clice modularize` turns the third-party libraries of a program into C++20 named modules without touching a source file. Each library's headers stay the source of truth: a generated module interface unit includes them in its global module fragment and exports every namespace-scope name they declare. The program keeps its `#include` directives; a directory first on its include path turns the wrapped headers into empty files, and a prelude it force-includes imports the modules and replays the macros their headers defined.

The program's own code can follow: modules the partition marks for rewriting have their files rewritten in place into module units, headers into partitions and includes into imports.

**Usage**: `clice modularize --partition <file> --out <dir> [--std <dir>] [--scope <glob,...>] [--workspace <dir>] [--configuration <tag>]`

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

- A module with no flag is wrapped: modularize writes its interface unit.
- `"external": true` marks a module an existing interface stands for. Only `std` is supported: with `--std` naming libc++'s module sources (`share/libc++/v1`), it is imported as `std.compat`, and the standard headers `std.cppm` includes are emptied.
- `"textual": true` keeps a module headers, as the C library and the compiler's own headers stay beside `import std`. What the program reaches of them only through emptied headers is included or replayed in the prelude.
- `"provides": "std.compat"` names the module exporting a textual module's names, so only what it leaves out needs a real header.
- `"rewrite": true` marks a module of the program to rewrite into a named module, see [rewriting the program](#rewriting-the-program). `"primary"` places its primary interface unit; by default it is `module.cppm` in the deepest directory holding the module's headers, or its files when it has none.

One module per library and one module for all of them are both a partition away; the scope has to take in every file the partition names.

## Rewriting the program

A rewritten module becomes one named module, its name the module's:

- Each header becomes a partition, `<stem>.cppm` beside it, named by its path under the primary interface's directory (`src/support/format.h` is `:support.format` under `src/`). A header files of other modules use is an interface partition the primary interface re-exports; one only the module's own files use is an implementation partition.
- Each source becomes an implementation unit, `module <name>;`, and a `main` it defines moves into `extern "C++"`, which keeps it attached to the global module.
- An include of a header of the same module imports that partition, so inside a module an edit rebuilds what including the header did; an include of a header of another rewritten module imports that module. A source names its module's interface partitions through the primary interface, which every implementation unit imports.
- Includes of wrapped headers go, the prelude imports their modules; every rewritten file includes the prelude first, in its global module fragment. Headers that stay headers stay included there.
- A declaration of another module's entity on a line of its own (`class Foo;`) goes, and the file imports the owner's module instead; an anonymous namespace of a header is dissolved into the enclosing one.
- Macros a header defines for other files move to `<stem>.macros.h` beside it, its directives without the includes, which every file using them includes.
- A source defining what a header of another rewritten module declares joins that module, since a definition is attached where its declaration is; a source of a module that is not rewritten and includes a rewritten header imports its module instead and stays a plain translation unit.

Which modules to rewrite and how coarse they are is the partition's choice: one module for the whole program keeps every header a partition, imported file by file; a module per directory gives each directory an interface but rebuilds every importer of a module when one of its interface partitions changes. [`clice analyze modules`](./analyze.md#modules) weighs both before the rewrite.

## Output

Under `--out`:

- `<module>.cppm` per wrapped module, and `<module>.macros.h` with the macros its importers need, in definition order.
- `mirror/<module>/`: an empty file for each header files of other modules include, by the name they include it with; `mirror/std/` for the standard headers.
- `prelude.h`: the C library headers still needed, `import std.compat;`, every import, every macro header.

Files whose content did not change keep their timestamps. The files a previous run wrote that this one no longer produces are removed; `--out/.modularize` lists what a run wrote, and nothing else under `--out` is touched.

The rewritten files are written in place, and the headers their partitions replace are deleted.

On stdout, the build plan. `wrapping` holds the module sources, mirrors and the prelude relative to `--out`, libc++'s sources and each library's include roots as found; `rewriting` the rewritten modules' units, workspace-relative:

```json
{
  "wrapping": {
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
  },
  "rewriting": {
    "modules": [
      {
        "name": "app.core",
        "primary": "src/core/module.cppm",
        "interfaces": ["src/core/text.cppm"],
        "partitions": ["src/core/detail.cppm"],
        "sources": ["src/core/text.cpp"],
        "imports": []
      }
    ],
    "importers": ["src/main.cpp"],
    "macros": ["src/core/text.macros.h"],
    "removed": ["src/core/detail.h", "src/core/text.h"],
    "moved": [],
    "warnings": []
  }
}
```

The build compiles `stdSources` and each module in order, each with its library's include roots and its `mirrors` first on the include path, and adds `mirrors` and `-include prelude.h` to every program compilation the rewrite leaves untouched. A rewritten module compiles its primary interface, `interfaces` and `partitions` as module interface units and its `sources` as implementation units, with the workspace root on the include path for the prelude; `moved` lists the sources that joined another module, `path=module`.

## Known Limitations

- Module units must be built with full BMIs (`-fno-modules-reduced-bmi` on clang): a reduced BMI drops the global module fragment's declarations the purview never names, partial specializations among them.
- A header holding an internal-linkage entity the program names stays textual: no module can export it. The prelude does not include such a header; a program file that reached it only through the library's emptied headers includes it itself.
- A header other files include by a name relative to their own directory (`"../foo.h"`) cannot be emptied; modularize warns about it.
- Every translation unit using a wrapped library has to see it through the module; mixing textual includes of the same headers with the import breaks on redeclarations clang cannot merge.
- Every program compilation imports every module and replays every macro header, whichever libraries it included before.
- The standard library module is libc++'s.
- Only the files the index holds are rewritten: a source no compilation of the indexed configuration enters, as another platform's, keeps its includes.
- Clang does not take the global module fragment of an implementation partition as reachable from a unit importing it through another partition, though the standard has that unit import it too. A rewritten file therefore includes the headers that stay headers which such partitions include, and parses them again.
- An include of a rewritten header under a preprocessor condition becomes an unconditional import.
- A header that stays a header cannot include a rewritten one; modularize warns about it.
- A forward declaration of an entity outside the scope moves to the global module fragment under the namespaces of its qualified name; an inline namespace among them, such as one a macro opens, is not reproduced.
