# Coming from clangd

A project set up for clangd often carries workarounds for what clangd cannot read on its own. clice reads most of them without help; this page says which ones can go, and where the settings of a `.clangd` file go in `clice.toml`. clice does not read `.clangd`.

## Workarounds to Drop

### Header Entries in the Compilation Database

clangd compiles a header without an entry by guessing a command from a nearby source, so some projects add the headers to `compile_commands.json`, by hand or with a tool. clice compiles a header in the context of a source that includes it, with that source's command and what the source declares before the include (see [header contexts](../features/header-contexts.md)); a header nothing includes yet borrows the command of a unit nearby. An entry of the header's own takes precedence over that context, and makes the header a unit of its own that the background index compiles beside the sources including it. Generate the database without them; a build system that writes them itself has them kept out of the index by a rule:

```toml
[[rules]]
patterns = ["**/*.h", "**/*.hpp"]
background_index = false
```

### `--query-driver`

clangd asks a compiler for its system headers and target only when `--query-driver` allows it, which a cross compiler or a GCC outside the default paths needs. clice asks the compiler of every command: its target, standard library and system headers, predefined macros and default standard. A rule's `query_toolchain = false` turns that off for the files it matches. A launcher in front of the compiler — ccache, sccache, distcc or icecc — is taken off the command first, and the compiler after it is the one asked.

### Flags Removed for clang

A `.clangd` often removes from GCC's, cl's or nvcc's commands what clang cannot read. clice reads those commands as their compiler does:

- The warnings GCC knows and clang does not (`-Wlogical-op`, `-Wuseless-cast`) are left out, and the warnings GCC, cl or nvcc make errors (`-Werror`, `-Werror=shadow`, `/WX`) stay warnings: clang warns of things those compilers do not.
- What only shapes the generated code, or writes a file beside the object — the LTO job count, GCC's module mapper and its dependency scan outputs, cl's module interface outputs — is left out.
- GCC's `-fmodules` reads as C++20 modules, a cross GCC's `-march` and `-mabi` as clang takes them, and nvcc's options as nvcc reads them, those for the host compiler included.
- An option clang does not know at all is left out of the compile, with a warning in the log.

### A Symlinked `compile_commands.json`

clangd looks for `compile_commands.json` in the directories above a file and in their `build/` directories, so a build tree elsewhere is often linked into the project's root. clice loads every `compile_commands.json` in the project's root directory and its immediate subdirectories, whatever their names (`build/`, `cmake-build-debug/`), and a link to one it finds already is the same database. One deeper or elsewhere is named by a rule:

```toml
[[rules]]
compile_commands = ["out/release"]
```

Two build directories found together compile each file under the first, and offer the other as a context to switch to; tagged rules turn them into [build configurations](./configuration.md#switching-configurations).

### Unity Builds Turned Off

A unity build lists only the generated files including the sources, so clangd finds no command for a source and guesses one. clice compiles a source no database lists in the context of the unity file including it, the way it compiles a header.

## `.clangd` Settings in `clice.toml`

| `.clangd` or clangd's option             | `clice.toml`                                                 |
| ---------------------------------------- | ------------------------------------------------------------ |
| `If: PathMatch`                          | a rule's `patterns`                                          |
| `CompileFlags: Add`                      | a rule's `append`                                            |
| `CompileFlags: Remove`                   | a rule's `remove`                                            |
| `CompileFlags: CompilationDatabase`      | a rule's `compile_commands`                                  |
| `Index: Background: Skip`                | a rule's `background_index = false`                          |
| `--background-index=false`               | `background_index = false` on a last rule without `patterns` |
| `Diagnostics: Suppress`                  | `[diagnostics] suppress`                                     |
| `Diagnostics: ClangTidy`, `--clang-tidy` | the project's `.clang-tidy`; `[diagnostics] clang_tidy`      |
| `InlayHints`                             | `[inlay_hints]`                                              |
| `Hover: ShowAKA`                         | `[hover] show_aka`                                           |
| `Completion: AllScopes`                  | `[code_completion] all_scopes`                               |
| `--function-arg-placeholders=0`          | `[code_completion] arguments = "delimiters"`                 |
| `--limit-results`                        | `[code_completion] limit`                                    |
| `--limit-references`                     | `[references] limit`                                         |
| `-j`                                     | `[workers] background`                                       |
