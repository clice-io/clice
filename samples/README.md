# Sample projects

Hand-written C++ workspaces every suite shares: the integration tests
(`tests/integration`), the unit tests (`samples_dir()`), the editor tests,
the smoke replays, and anyone trying clice by hand. Nothing runs in this
tree: users work on a copy.

```sh
node tools/sample.ts shapes/headers /tmp/shapes   # copy + compile_commands.json
```

## What is here

| Sample           | What it is                                                                                                                                | Used for                                                                                                                                                |
| ---------------- | ----------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `tiny`           | One `main.cpp`, no includes                                                                                                               | Anything that only needs a file that compiles                                                                                                           |
| `shapes/headers` | A small geometry library, a program and a test program on headers                                                                         | Scenarios across files: headers and their sources, a header two units include with different `-D`, templates, a class hierarchy, a call chain, a C unit |
| `shapes/modules` | The same library on C++20 modules, file for file                                                                                          | The same scenarios on modules: interfaces, partitions, implementation units, importers                                                                  |
| `contexts`       | Headers that compile only inside their includers: a two-level chain over an earlier include, and a header two sources prepare differently | Header contexts: synthesized preambles, listing, switching and resetting hosts                                                                          |
| `stdlib`         | One unit on `<map>`, `<string>`, `<vector>`                                                                                               | A real standard-library preamble; kept apart so no other sample pays for it                                                                             |
| `layouts/*`      | Directory layouts with hand-written databases or `clice.toml`                                                                             | Database discovery, configurations, nested projects                                                                                                     |
| `modules/*`      | One module topology each: chains, diamonds, cycles, partitions                                                                            | Module dependency handling                                                                                                                              |

## Manifests

Each sample carries a `project.json`:

```json
{
  "args": ["-Iinclude"],
  "cxx": ["-std=c++23"],
  "c": ["-std=c17"],
  "units": { "src/circle.cpp": ["-DSHAPES_EXACT"], "app/main.cpp": [] },
  "files": { "circle": "include/shapes/circle.h", "main": "app/main.cpp" }
}
```

- `units` are the translation units with their own arguments; a copy gets
  a `compile_commands.json` with its real root and the test toolchain's
  compilers. A unit of several configurations lists one argument list per
  configuration. A sample that ships its own databases (`layouts/*`) has
  no `units`.
- `files` gives files a role name. Tests name files by role
  (`s.file("circle")`) and positions by a unique snippet of the file
  (`at(file, "are|a(")`), never by line or column, so a sample can grow
  without breaking its users. `serve.each` runs one case on several
  samples whose roles match (`shapes/headers` and `shapes/modules`).

## Adding to a sample

- Need a structure no sample has (a header nobody includes, a unit that
  only compiles with a flag)? Add it to the sample whose story it fits,
  give it a role, and keep the sample compiling clean. A structure only one
  case needs belongs in that case instead.
- A test that tests the content of a file — a diagnostic's shape, an odd
  construct — writes that file itself (`serve.files`, or `files` over a
  sample). Every other file it needs comes from a sample.
- Keep samples small and free of the standard library (except `stdlib`):
  every case that copies one compiles it, and indexes it when indexing is
  on.
- `npm run check` and the anchor check (`tests/tools/anchors.test.ts`)
  catch a snippet a change made ambiguous or removed.
