# Configuration Guide

clice needs no configuration once it finds a compilation database (see [Project Setup](./quick-start.md#project-setup)). A `clice.toml` says what the database does not: the files it does not list, the flags to change, the builds to switch between, the code to treat apart. This page goes through the common cases; the [Configuration Reference](./configuration.md) lists every option.

## The Configuration File

clice reads `clice.toml` in the project root, or `.clice/config.toml` when there is none, once, when the server starts; saving the file makes the server ask the editor to restart it. Settings for every project — the workers, the features' preferences, rules for the files every project has — go in the user's file, `clice/config.toml` in `$XDG_CONFIG_HOME` where it is set to an absolute path, else in `~/.config` (`%APPDATA%` on Windows), under the project's; a rule there naming databases or a default command turns the discovery of databases off in every project, so those stay in the project's files. A developer's own settings for one project go in `.clice/local.toml`, over the project's, which Git does not see while the cache directory is the default one, `.clice`. An editor can send settings too, in `initializationOptions`, and those take the place of the same settings in the files. `clice inspect config` shows which files a project reads and where each setting it changes comes from.

<!-- BEGIN GENERATED ITEMS: config/loading -->

<!-- BEGIN CAPABILITY: supported -->

**Changes restart the server**

clice reads `clice.toml` when the server starts. Saving it makes the server ask the editor to restart it, and the edit applies in the new server

The rule gains the macro while the server runs; once the editor restarted the server, `main.cpp` compiles with it.

```case
tests/config/loading/04_edited_file
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

<!-- BEGIN GENERATED ITEMS: config/layers -->

<!-- BEGIN CAPABILITY: supported -->

**Editor settings over the file**

Rules the editor sends replace the rules of `clice.toml`

`clice.toml` gives `main.cpp` the macro it uses, and the file compiles. An editor that sends its own `rules` in its settings replaces the file's rules rather than adding to them, so here the macro is gone.

```case
tests/config/layers/01_editor_overrides
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**The user's file**

Settings for every project go in the user's configuration file, under each project's own

`clice/config.toml` in the user's configuration directory (`~/.config` on Linux and macOS, `%APPDATA%` on Windows, `$XDG_CONFIG_HOME` wherever it is set to an absolute path) applies to every project, under its `clice.toml`. Here the user's rule and the project's both give `main.cpp` a macro, and the file compiles; `clice inspect config` lists the files read and where each setting comes from.

```case
tests/config/layers/03_user_file
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**The local file**

`.clice/local.toml` holds a developer's own settings for a project whose `clice.toml` is committed

The project's `clice.toml` compiles `main.cpp` as C++17 with a macro; the local file, which the cache directory's `.gitignore` keeps out of Git, asks for C++20 and takes the macro away. Its rules come before the project's, so they decide first, and their flags come after, so they win on the command line: the concept compiles and the `#error` stays out.

```case
tests/config/layers/04_local_file
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

Most of what follows is written as rules. A `[[rules]]` entry names files by `patterns`, relative to the project root, and sets fields for them. Every rule matching a file applies, in order: their `append` and `remove` add up, and each other field is decided by the first rule setting it, so a rule for some files goes before a rule for all of them, and a rule without `patterns` goes last. Across files, the local file's rules come first, then the project's, then the user's, while their `append` and `remove` edit the command the other way round, the user's first: the more specific file decides, and its flags come last. `clice command <file>` shows what the rules make of a file: each command its databases list for it, with the rules' edits, and the value of each per-file setting with the rule deciding it. In VS Code, Show Compile Command shows the command an open file compiles with, wherever it comes from.

## Compile Commands

clice loads every `compile_commands.json` in the project root and its immediate subdirectories, whatever their names (`build/`, `cmake-build-debug/`), and those between a file you open and the root. A database elsewhere is named by a rule, and naming one turns that discovery off:

```toml
[[rules]]
compile_commands = ["out/release"]
```

Several databases rank in the order given: a file listed in more than one compiles under the first by default, and can be switched to the others.

Sources no database lists compile with a rule's `default_command`, written as on a command line without the source file, and those it matches on disk join the background index. A header takes the command of a source including it, and the `default_command` only when none does:

```toml
[[rules]]
patterns = ["src/**", "include/**"]
default_command = "arm-none-eabi-gcc -std=c23 -mcpu=cortex-m4 -Iinclude"
```

`append` adds flags to the commands of the files a rule matches, and `remove` takes flags out: a flag with its value (`-DDEBUG`), every value of a flag (`-I*`, or `["-include", "*"]`), or every flag of a prefix (`-W*` takes `-Wall` and `-Wextra`). A rule removes before it appends, and a later rule's `remove` cancels an earlier rule's `append`:

```toml
[[rules]]
patterns = ["tests/**"]
append = ["-DTEST_MODE"]

[[rules]]
patterns = ["legacy/**"]
remove = ["-W*"]
```

<!-- BEGIN GENERATED ITEMS: project/rules -->

<!-- BEGIN CAPABILITY: supported -->

**Files without a database**

Files a rule matches compile with its default command, and files created later join the index

`src/main.cpp` compiles under the rule's command, with no note that the command was guessed. A file created while the server runs compiles the same way at once and is indexed at the next look at the disk.

```case
tests/project/rules/01_default_command
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Databases in priority order**

A file several databases list takes its command from the first one, and the others stay on offer as contexts

Database `a` lists `main.cpp` with `-DFROM_A`; database `b` lists it with `-DFROM_B` and also lists `other.cpp`. `main.cpp` compiles with the command from `a` and can be switched to the one from `b`, while `other.cpp` takes its command from `b`.

```case
tests/project/rules/03_database_order
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Patterns start at the root**

A relative pattern matches paths under the project root, where `clice.toml` sits

The rule appends `-DFEATURE` to the files under `src/`: `src/main.cpp` compiles, while `other.cpp`, beside `clice.toml`, misses the macro.

```case
tests/project/rules/05_relative_patterns
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**Remove flags from commands**

A rule takes flags out of a file's command, and a later rule's remove cancels an earlier append

The database gives `main.cpp` both `-DBROKEN` and `-include prefix.h`, which the first rule removes, so it compiles. For `other.cpp` the second rule appends `-DFEATURE` and the third removes it again.

```case
tests/project/rules/09_remove_flags
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Several Builds

Two build directories found together compile each file under the first, and offer the other as a context to switch to. Tagged rules make them build configurations instead, each with an index of its own:

```toml
default_configuration = "debug"

[[rules]]
configuration = "debug"
compile_commands = ["build/debug"]

[[rules]]
configuration = "release"
compile_commands = ["build/release"]
```

The `configuration` tags on rules form a menu, and one tag is active per project; untagged rules always apply. The active one is, in priority order, the `--configuration <tag>` argument, which `clice serve` and every command working on a project take, the persisted selection, or `default_configuration`. A selection is made from the editor — in VS Code **Switch Build Configuration** in an editor's context menu or the Command Palette, which lists the configurations and marks the active one, other clients call `clice/switchConfiguration` — and is stored in `state.json` under `cache_dir`, never in `clice.toml`; it takes effect when the server is started again, which the server asks the editor to do, as for an edited configuration file. Each configuration keeps its own index under `cache_dir`, so switching back and forth never reindexes what a configuration already indexed. The tags are also how the batch commands choose an index: `clice index --configuration release` builds the release index, and `clice index stats` reports the index of the configuration it resolves the same way.

Some build systems choose a build by pointing a symlink at its directory instead. A rule with `resolve_symlinks = true` tells builds apart by the real path its `compile_commands` resolve to, through symlinks and Windows junctions: each build the link points to keeps an index of its own, the running server asks to be restarted on that build's index when the link is pointed elsewhere, and a build pointed at again finds its index where it left it. A configuration whose rules differ only in naming that build's databases directly shares its index. In VS Code, Switch Build Configuration says which build the active configuration's links resolve to, and marks the configuration sharing its index. An index is kept until deleted: each is a directory under `cache/v<N>/index` in `cache_dir`, named after the configuration and the last directories of the build's path.

<!-- BEGIN GENERATED ITEMS: project/configurations -->

<!-- BEGIN CAPABILITY: supported -->

**One command per board**

Tagged rules form a menu of build configurations, and a switch applies when the server starts again

Each board's rule gives its own default command, and `board-a` is the default configuration. Under it the header's `BOARD_B` branch is inactive; after switching to `board-b` and restarting, the `BOARD_A` branch is.

```case
tests/project/configurations/03_board_defaults
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**One index per linked build**

A rule resolving symlinks keeps an index for each build its link points to, and follows the link when it moves

The build system points `out/current` at the build it chose. Pointed at `out/debug`, which the `debug` configuration names too, the server switches to that configuration's index; pointed back at `out/release`, the release index serves again and nothing is indexed anew.

```case
tests/project/configurations/07_linked_build
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## Third-Party Code

Code you only call, such as the libraries vendored under `third_party/`, needs neither its function bodies parsed in every compile nor lint and formatting:

```toml
[[rules]]
patterns = ["third_party/**", "external/**"]
skip_function_body = true
lint = false
format = false
```

`skip_function_body = true` makes every compile but the file's own skip the function bodies of the files it matches, templates included, in the editor and in the background index alike: the units of your project then parse only the declarations of the libraries they include, and a module of the library they import is built with its bodies skipped too. Function templates that forward their arguments, such as `std::make_unique`, are still parsed, and so are `constexpr` and `consteval` functions and those returning `auto`.

What skipping costs: the errors inside those bodies are not reported, nor the errors your code causes when it instantiates their templates, and the names their bodies use are not indexed — references and call hierarchies stop at their declarations. `skip_function_body = false` turns skipping off everywhere, the editor's own included: by default the editor skips the bodies of the headers at the top of an open file and the background index parses them, reporting what it finds there on a file compiled under its own database entry.

`lint = false` keeps the files out of `clice lint` and the editor's clang-tidy, and `format = false` out of `clice format`.

## A Faster First Index

The answers that reach beyond the files you have open — references, call and type hierarchies, workspace symbols, rename, and hover or navigation on a file not compiled yet — come from clice's index of the project. When the server starts, the background index compiles every translation unit of the project its saved index does not hold as current: each file of its compilation databases, the sources a rule's `default_command` covers, and the files opened earlier that no database lists. It starts `[workers] background_delay_ms` after work is queued, runs on the background workers, and gives way to the requests of the files you work on. A header is indexed through the units that include it, once for each way they include it, so a header compiled differently by different sources answers for each of those contexts.

The index is saved in the cache directory, every five minutes while it is built and when the run ends, and the next server, or `clice index` run, compiles again only the units whose files changed since. The first index of a big project is therefore a cost paid once: it compiles every unit, and a run interrupted resumes where it stopped. [`clice index`](../cli/index.md) pays it ahead from the command line; while a server of the same clice version serves the project, it asks that server to do it. The session log says where the time goes: a line for each unit with how long it took, and with `CLICE_PERF=1` in the environment a `[perf:index]` line per unit splitting that time into indexing and merging, and one for the whole run. Five things shorten it:

- **More workers.** `[workers] background` is how many units the server compiles at once: by default half the machine's physical cores, at least 2, and never more than `[workers] max`, which defaults to all of them. On a machine with many cores and memory to spare, raise it. `clice index` compiles `--jobs` units at once, by default as many as the machine has CPUs.
- **Enough memory.** A unit starts only when the machine's memory leaves room for it, and the newest one stops when memory runs short, so on a machine short of memory more workers do not help: the log says when background work waits for memory. A unit that does not fit even alone is named by `clice index`, with the memory its worker reached.
- **Fewer units.** A rule with `background_index = false` keeps the files it matches from being compiled as units of their own: tests, examples, benchmarks or generated sources you do not navigate in. A header they include is still indexed through the other units including it, and a module unit or the unit writing a precompiled header still compiles when another unit needs its module or header. A compilation database that lists headers, as some generators write them, makes each a unit of its own beside the sources including it; a rule over `**/*.h` with `background_index = false` keeps those out.
- **Skipped bodies.** [Third-party code](#third-party-code) with `skip_function_body = true` costs your units only its declarations.
- **No real-time scanning, on Windows.** Microsoft Defender scans a file the first time a program reads it after it changed, and clice reads every source and header of the project when it starts: on a large project whose files Defender has not scanned yet, the start takes many times as long. Excluding the project's directory from real-time scanning (Windows Security, Virus & threat protection settings, Exclusions) avoids it, and leaves what that directory holds unscanned by Defender.

```toml
[workers]
background = 16

[[rules]]
patterns = ["third_party/**", "external/**"]
skip_function_body = true
background_index = false

[[rules]]
patterns = ["benchmarks/**", "**/*.h"]
background_index = false
```

One process writes a project's index at a time. A second server on the same project, such as a second editor window, waits up to five seconds for the first to release it, time enough for a restarting server's predecessor to finish saving; if it is still held, the second serves nothing of that project and says which process holds the index.

## Library Internals

For each file the index records its declarations, definitions and references, what calls what, what derives from what, and the template instantiations the units make. Inside a function body it records the names the body uses, except in system headers (the standard library, an `-isystem` directory): their bodies, and the instantiations of the templates written there, are left out, while their declarations, types and templates are indexed. To navigate inside such a library — find the callers of a function within it, or the instantiations of its templates — let a rule index its bodies:

```toml
[[rules]]
patterns = ["/usr/include/c++/**"]
index_function_body = true
```

The index grows with every body it records, so this is best kept to the libraries you read.

## Coming from clangd

A project set up for clangd often carries workarounds for what clangd cannot read on its own. clice reads most of them without help; this section says which ones can go, and where the settings of a `.clangd` file go in `clice.toml`. clice does not read `.clangd`.

### Header Entries in the Compilation Database

clangd compiles a header without an entry by guessing a command from a nearby source, so some projects add the headers to `compile_commands.json`, by hand or with a tool. clice compiles a header in the context of a source that includes it, with that source's command and what the source declares before the include (see [header contexts](../features/header-contexts.md)); a header nothing includes yet borrows the command of a unit nearby. An entry of the header's own takes precedence over that context, and makes the header a unit of its own that the background index compiles beside the sources including it. Generate the database without header entries; if your build system writes them anyway, keep them out of the background index with a rule (see [Fewer units](#a-faster-first-index)).

### `--query-driver`

clangd asks a compiler for its system headers and target only when `--query-driver` allows it, which a cross compiler or a GCC outside the default paths needs. clice asks the compiler of every command: its target, standard library and system headers, predefined macros and default standard. A rule's `query_toolchain = false` turns that off for the files it matches. A launcher in front of the compiler — ccache, sccache, distcc or icecc — is taken off the command first, and the compiler after it is the one asked.

### Flags Removed for clang

A `.clangd` often removes from GCC's, cl's or nvcc's commands what clang cannot read. clice reads those commands as their compiler does:

- The warnings GCC knows and clang does not (`-Wlogical-op`, `-Wuseless-cast`) are left out, and the warnings GCC, cl or nvcc make errors (`-Werror`, `-Werror=shadow`, `/WX`) stay warnings: clang warns of things those compilers do not.
- What only shapes the generated code, or writes a file beside the object — the LTO job count, GCC's module mapper and its dependency scan outputs, cl's module interface outputs — is left out.
- GCC's `-fmodules` reads as C++20 modules, a cross GCC's `-march` and `-mabi` as clang takes them, and nvcc's options as nvcc reads them, those for the host compiler included.
- An option clang does not know at all is left out of the compile.

### A Symlinked `compile_commands.json`

clangd looks for `compile_commands.json` in the directories above a file and in their `build/` directories, so a build tree elsewhere is often linked into the project's root. clice finds the databases in every immediate subdirectory of the root, whatever its name, and a link to one it finds already is the same database; one deeper or elsewhere is named by a rule (see [Compile Commands](#compile-commands)).

### Unity Builds Turned Off

A unity build lists only the generated files including the sources, so clangd finds no command for a source and guesses one. clice compiles a source no database lists in the context of the unity file including it, the way it compiles a header.

### `.clangd` Settings in `clice.toml`

| `.clangd` or clangd's option             | `clice.toml`                                                            |
| ---------------------------------------- | ----------------------------------------------------------------------- |
| `If: PathMatch`                          | a rule's `patterns` (globs, not regular expressions)                    |
| `CompileFlags: Add`                      | a rule's `append`                                                       |
| `CompileFlags: Remove`                   | a rule's `remove`                                                       |
| `CompileFlags: CompilationDatabase`      | a rule's `compile_commands`                                             |
| `Index: Background: Skip`                | a rule's `background_index = false`                                     |
| `--background-index=false`               | `background_index = false` on a last rule without `patterns`            |
| `Diagnostics: Suppress`                  | `[diagnostics] suppress`                                                |
| `Diagnostics: ClangTidy`, `--clang-tidy` | the project's `.clang-tidy`; `[diagnostics] clang_tidy`                 |
| `InlayHints`                             | `[inlay_hints]`                                                         |
| `Hover: ShowAKA`                         | `[hover] show_aka`                                                      |
| `Completion: AllScopes`                  | `[code_completion] all_scopes`                                          |
| `--function-arg-placeholders=0`          | `[code_completion] arguments = "delimiters"`                            |
| `--limit-results`                        | `[code_completion] limit`                                               |
| `--limit-references`                     | `[references] limit`                                                    |
| `-j`                                     | `[workers] background` and `[workers] max`                              |
| clangd's user `config.yaml`              | the user's file (see [The Configuration File](#the-configuration-file)) |
