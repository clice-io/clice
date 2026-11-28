# Command Resolution

## Background

A language server learns how to compile each file from the compilation database, usually a `compile_commands.json` the build system writes:

```bash
g++ -std=c++20 -O2 -g -I../include -DNDEBUG -c src/foo.cpp
```

It cannot run that command as it stands:

- **It is a driver command.** `g++` picks a frontend, a standard library and system headers; clice runs Clang's frontend, which needs all of that spelled out — the target, the system include directories, the default language standard. None of it is in the database: the compiler supplies it.
- **It carries options that change nothing a language server sees**: `-c`, `-o`, dependency files, `-ffunction-sections`.
- **It repeats itself.** Ten thousand units of one target share a compiler and its options and differ in include paths and macros.

What is missing hurts most: without the system headers, `#include <vector>` is "file not found" — consistently among clangd's most reported problems ([clangd#1262](https://github.com/clangd/clangd/issues/1262), [clangd#1691](https://github.com/clangd/clangd/issues/1691)). clangd asks a compiler only when `--query-driver` names it, which the user has to find out and configure ([clangd#1219](https://github.com/clangd/clangd/issues/1219)). clice asks every command's compiler, recognizes its family, and keeps the number of questions small.

## Classifying Options

Each command is parsed once, with Clang's own option table, by option ID: every spelling of an option — joined, separate, cl's `/` — classifies alike. Each option is either left out or kept in a category:

- **Left out**: what only the build reads — outputs, compile mode, dependency files, timings, the assembler's and the linker's options, GCC's module mapper, cl's module interface outputs, Clang's header modules and the layering check they enforce — and what only shapes the generated code (`-ffunction-sections`, `-fno-plt`, the paths debug information records). Options that look like code generation but change the preprocessor stay: `-O` defines `__OPTIMIZE__`, `-fPIC` defines `__PIC__`, `-fsanitize=address` changes what `__has_feature` answers, and `-g` decides `__GCC_HAVE_DWARF2_CFI_ASM` in a C file without unwind tables.
- **Diagnostics**: warning options. They change which diagnostics show, nothing else.
- **User content**: what one file needs — include paths, macros, forced includes. It does not change what the compiler answers about itself.
- **Semantic**: the rest — `-std`, `--target`, `-march` — which the compiler's answers depend on.
- **Precompiled**: the PCH a command reads or writes, and whether it writes a module, which clice builds itself.
- **Input**, the file, in a slot of its own; and **unknown** options, which count in the command's identity.

The parsed commands live in one table, each once. Everything downstream — querying, rules, rendering — reads the structured form, and a command's identity is its **command hash**: every argument it keeps, the input's position and the working directory. The index records each unit's command hash, and the user's choice of an entry names it.

The classification is also what keeps questions few: a compiler's answer does not depend on the user content, so files differing only in it share one answer. Tens of thousands of files come down to a few dozen questions.

## Rules, Members and Hosts

The rules of `clice.toml` decide, under the active build configuration, what the databases' entries become; every consumer — the editor, the dependency scan, the background index, the command line — asks the project, never a database directly.

- **Entries.** A file's entries in build order: the databases its matching rules name first, then the other rules', then the discovered ones. The first is the default; the user can pick another. A rule decides priority only: no entry disappears because a pattern does not name its file.
- **Edits.** Every matching rule's `remove` and `append`, in declaration order. A command a file takes from another — a header from its host — is edited by the rules of both files, so a header sees the macros its host compiles with.
- **Members.** The translation units of the build: the files with entries, the sources a rule's `default_command` covers, and the provisional members (below). A header a database lists is a member too, and is indexed as a unit of its own.
- **Hosts.** A header without a command of its own compiles in a unit that includes it, of a language the header can be part of (never C for a `.hpp`). The candidates rank by the databases the header's own rules name, then a shared stem, then the same directory, then the distance by path; the first whose compile actually enters the header hosts it. See [Compilation Context](compilation-context.md).
- **Borrowed commands.** A file with neither an entry, a host nor a `default_command` borrows a nearby unit's command of its language: one in its directory, the one sharing its stem first, else for a header the unit whose include directories contain it, else the nearest. It is labeled so, and its diagnostics about missing files say the command was borrowed.
- **Provisional members.** A source of the project the user opens or saves that no database lists and no unit includes — one created after the database was written, or one of a project without a database — joins the build, under a borrowed command or clice's own when there is none to borrow: it is indexed, provides its module and hosts the headers it includes. A source a unit includes, such as the part of a unity build, compiles in that unit's context instead. The record outlives restarts; a database that lists the file takes over.

### Discovery

A configuration that declares no database and no `default_command` discovers them: the `compile_commands.json` of the project root and of its immediate subdirectories, and when a file is opened, those in the directories between it and the root. A directory with a `clice.toml`, `.clice/config.toml` or `.clice/local.toml` of its own is another project. A link to a database found already is the same database. The server keeps looking on its poll, so a database generated later loads when it appears. As soon as a rule declares a source, discovery is off: the declaration is the whole intent.

## Asking the Compiler

Each compiler family is asked in its own way, once per distinct question, and the answer is cached:

- **GCC-style drivers** (GCC, clang, Apple clang, emcc, zig, cross and bare-metal GCC) preprocess an empty file of the input's language with the command's own flags, verbosely and printing their macros (`-E -dM -v`). One run answers everything: a clang driver prints the frontend job clice runs; a GCC prints its target, its header search list and the options its specs add, and clang's driver builds the frontend command pinned to that target with GCC's search list. The macros give the default language standard. Whether a driver is clang or GCC is told by its answer, never by its name: `/usr/bin/c++` is clang on macOS.
- **cl and clang-cl** preprocess a file naming `_MSVC_LANG` and `_MSC_FULL_VER`, which give the standard and, for cl, the version; clang's driver in cl mode builds the command.
- **nvcc**'s dry run gives the CUDA root, the host compiler, nvcc's macros and its header directories; a GCC or clang host is asked as above, and clang's driver builds a CUDA command.
- **A compiler clice does not run** — a rule's `query_toolchain = false`, or one whose query fails — leaves the command to clang's own driver, which reads it as a clang installed under that name would; a failed query is shown at the top of each file it concerns.

What the answer cannot carry over is adjusted to clang. GCC's private include directory mixes GCC's versions of clang's builtin headers, which clang cannot parse, with headers only GCC has (`omp.h`): clice keeps the directory and hides the files clang's own stand in for; CPU and ABI names clang does not know are dropped; the warnings GCC, cl and nvcc make errors stay warnings, since clang warns of things those compilers never report; GCC's `-fmodules` means C++20 modules, not clang's header modules. A launcher in front of the compiler (ccache, sccache, distcc, icecc) is taken off first.

At startup, every distinct question of the build is asked in parallel during the dependency scan, so the first compiles find their answers cached.

## Search Paths

The frontend command's include directories are sorted into four tiers as Clang sorts them — quoted (`-iquote`), angled (`-I`), system and after (`-idirafter`) — and deduplicated the way Clang does: a directory both angled and system is searched where the system group puts it. The same order is what clice's own include search, the dependency scan and `#include` completion use, so `#include_next` and same-named headers resolve to the files the compiler would find.

## FAQ

- **Why does a GCC command parse as clang?** clice's frontend is clang. The macros naming the compiler stay clang's — `__clang__` is defined — because GCC's would send clang down branches written for GCC alone, full of builtins clang cannot parse. Everything that describes the build follows the real compiler: the target, the search list, the language standard, the macros its configuration adds.
- **Why ask by the name the command uses, not the resolved path?** A driver behaves by its name: `clang++` is a link to `clang` that compiles C++, and `arm-none-eabi-g++` names its target. Resolving the link would lose both.
- **Why must the search order match Clang exactly?** Which of two same-named headers a file includes, and where `#include_next` continues, follow from the order. Any difference would show the user other code than the compiler compiles.

## Known Limitations

- **Discovery never scans the tree.** A nested project's database loads when one of its files opens; a rule naming the database covers it from the start.
- **Only the editor adds unlisted files to the build.** A source that appears on disk any other way, that no database lists and no rule's `default_command` covers, stays outside the build until it is opened or saved, or a regenerated database lists it.
- **GCC-only code parses as clang sees it**: GCC-only builtins, `__float128` where clang lacks it, nested functions show clang's errors.
- **CUDA parses as clang's CUDA sees it**: code only nvcc accepts shows clang's errors.
- **A driver that answers neither as clang nor as GCC** — Intel's classic `icc`, other vendors' compilers — gets clang's defaults for the machine, and the error says so.
- **Only the common launchers are recognized.** Another launcher is taken for the compiler; strip it from the database.
- **Header maps are not read** by clice's own include search (dependencies, completion, links), though the compile applies them.
- **Cygwin symlinks among headers are not followed**: clice's clang reads the link file itself.
- **Commands written for other tools leave out the directories clice hides files in.** `clice command convert` writes the command without GCC's private include directory and another clang's builtin headers; `clice command <file> --json` lists what each hides.
