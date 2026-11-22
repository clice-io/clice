<p align="center"><img src="docs/public/logo.png" width="160" alt="clice"></p>

# clice

![C++ Standard](https://img.shields.io/badge/C++-23-blue.svg)
[![GitHub license](https://img.shields.io/github/license/clice-io/clice)](https://github.com/clice-io/clice/blob/main/LICENSE)
[![CI](https://github.com/clice-io/clice/actions/workflows/main.yml/badge.svg?branch=main)](https://github.com/clice-io/clice/actions/workflows/main.yml?query=branch%3Amain)
[![Documentation](https://img.shields.io/badge/view-documentation-blue)](https://docs.clice.io/clice/)
[![Discord](https://img.shields.io/badge/Discord-%235865F2.svg?logo=discord&logoColor=white)](https://discord.gg/PA3UxW2VA3)

A C/C++ language server written in modern C++, and more than a language server. Like clangd and ccls it is built on LLVM/Clang, but it is redesigned from the ground up to solve the problems their architecture has left unsolved for years. See [What is clice?](https://docs.clice.io/clice/guide/what-is-clice)

The story behind clice and how it was built: [clice v1.0 Released: Welcome to Modern C++](https://ykiko.me/en/articles/2105384023094834022/).

## Language Server

- **Multi-process compilation** — Compiles run in a pool of isolated worker processes, as in a modern browser. A compile that crashes or leaks on a corner case or on half-typed code costs one worker, not the server: navigation and highlighting keep working, and memory and compile priorities are managed across the pool.

  ![crash-vs-clangd](https://github.com/user-attachments/assets/284ba36e-b941-4657-a5ac-b11f783f5e53)

- **Modules and a shared cache** — Full C++20 named module support from a `compile_commands.json` listing the module files: clice scans the dependencies and builds the modules needed on demand, whatever the build system or compiler, with completion, highlighting, navigation and hover on modules. PCHs and PCMs are cached on disk and reused across sessions.

  ![modules-vs-clangd](https://github.com/user-attachments/assets/fd526ad2-76a5-487b-bbad-da3f9276e5a8)

- **Compilation contexts** — Non-self-contained files work out of the box: a header compiles in the context of a file including it, detected when it is opened, and you can pick another, or another compile command for a source. Build configurations group compiles, for example to switch between debug and release.

  ![contexts-vs-clangd](https://github.com/user-attachments/assets/84e9296f-24b7-4a11-a152-a72d110a31bc)

- **Better template support** — A pseudo-instantiator works out what dependent names refer to without concrete arguments, and a template instantiated once also answers through that instantiation, such as the `auto` parameter of a lambda passed to a `ranges` call. A template is a duck-typed interface and each instantiation one of its implementations: go to implementation, references and the call hierarchy on a dependent call list what each instantiation calls.

  ![templates](https://github.com/user-attachments/assets/51bb44a1-41cd-4cf7-aaa0-e9045024715f)

  ![implementations](https://github.com/user-attachments/assets/1b927e3f-c63b-4f9c-b8a0-f4e8423d36db)

- **Works out of the box** — Most projects need nothing but a `compile_commands.json`: clice detects the environment of 50+ toolchains, nvcc, MSYS2, Cygwin and zig cc among them, and reconciles the compiler actually used with Clang, with no flags to add by hand.

## Command Line

The same engine answers from the command line:

- `clice index` builds the project's persisted index ahead of time, resumably, so an editor starts warm.
- `clice lint` runs clang-tidy over the project's units in a process pool and reports a shared header's findings once; with `--index` the same parse builds the index.
- `clice format` formats the files the configuration and the compilation database name, in parallel, with clang-format.
- `clice command` shows the command a file actually compiles with after the rules and the toolchain query, or converts a `compile_commands.json` into resolved `-cc1` commands for other Clang tools.
- `clice query` reads the index on disk, no server needed, and answers in JSON: definitions, references, call graphs, type hierarchies, file dependencies.
- `clice refactor` rewrites the sources from the persisted index without compiling: symbol and macro renames over the whole project, or a batch of them with `--spec`.
- `clice analyze` reports the project's structure as JSON from the index, such as `analyze modules`: the module dependencies and cycles between directories, and the code standing in the way of modules.
- `clice modularize` wraps the third-party libraries a project uses as C++20 modules without touching a source file, or rewrites the project's own code into module units along `analyze`'s split: headers become partitions, `#include`s become `import`s. clice's own modules were made with it.

## Built with Modern C++

- C++20 [coroutines](https://github.com/clice-io/kotatsu/tree/main/include/kotatsu/async) throughout, on a structured-concurrency async library built on libuv: an event loop on the main thread, over many processes and threads.
- C++20 named modules and C++23 `import std`, self-hosted: `clice modularize` generates the module wrappers of the third-party libraries, and clice serves its own codebase.
- [Static reflection](https://github.com/clice-io/kotatsu/tree/main/include/kotatsu/codec) on templates and C++20 concepts for toml, json, bincode, blob and the command line, declared by annotations, with no boilerplate.
- Data-oriented, with almost no virtual functions beyond Clang's callback interfaces, on `std::format`, `std::expected`, `std::ranges` and `std::views`.
- A hermetic, reproducible Bazel build on the [xclang](https://github.com/clice-io/xclang) toolchain, cross-compiling to every platform, ThinLTO-optimized, depending on nothing but the system libraries.

## Getting Started

### Install

Download the latest binary from the [releases page](https://github.com/clice-io/clice/releases), or [build from source](https://docs.clice.io/clice/guide/build).

**Platforms:** Linux (x64, ARM64), macOS (x64, ARM64), Windows (x64, ARM64)

**Release channels:**

| Channel        | Version                           | Where to get it                                                                                                                                                                                                     |
| -------------- | --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Stable**     | even minor, e.g. `v1.0.0`         | [releases page](https://github.com/clice-io/clice/releases) + [VS Code Marketplace](https://marketplace.visualstudio.com/items?itemName=clice-io.clice) + [Open VSX](https://open-vsx.org/extension/clice-io/clice) |
| **Nightly**    | odd minor, e.g. `v1.1.2026112002` | [GitHub pre-releases](https://github.com/clice-io/clice/releases) + Marketplace and Open VSX pre-release channel (`Switch to Pre-Release Version` on the extension page)                                            |
| **Per-commit** | untagged                          | artifacts on the [latest green `main` CI runs](https://github.com/clice-io/clice/actions/workflows/main.yml?query=branch%3Amain+is%3Asuccess)                                                                       |

The versioning rule is the VS Code Marketplace convention: **even minor = stable, odd minor = pre-release**. Nightly patch numbers encode the UTC build time as `YYYYMMDDHH` (year, month, day, hour — e.g. `2026071902` is 2026-07-19 02:00 UTC), so newer is always higher; a nightly is published only when `main` gained commits, and only the newest 25 nightlies, none older than 30 days, are kept. Every published binary is the exact one that passed the full test suites — releases promote CI builds rather than rebuilding.

For the freshest bits, every green [`main` CI run](https://github.com/clice-io/clice/actions/workflows/main.yml?query=branch%3Amain+is%3Asuccess) attaches installable artifacts (binaries and platform vsix) to its run page, so a fix can be tried the moment it merges (GitHub login required for artifact downloads).

Each release also ships `*.symbols` packages: if clice crashes, attach the newest server log to an issue and the matching symbols let us reconstruct the exact stack. The log location is printed at startup in your editor's clice output panel (`Session log directory:` — by default under `<workspace>/.clice/logs/`).

### Editor Setup

| Editor                         | Setup                                                                                                                  |
| ------------------------------ | ---------------------------------------------------------------------------------------------------------------------- |
| **VS Code**                    | Install the [clice extension](https://marketplace.visualstudio.com/items?itemName=clice-io.clice) from the Marketplace |
| **Cursor, VSCodium, Windsurf** | Install the [clice extension](https://open-vsx.org/extension/clice-io/clice) from Open VSX                             |
| **Neovim**                     | With [nvim-lspconfig](https://github.com/neovim/nvim-lspconfig) on Neovim 0.11 or later: `vim.lsp.enable('clice')`     |
| **Zed**                        | Load `editors/zed` as a local extension                                                                                |
| **Other**                      | Any LSP client works — point it at `clice serve`                                                                       |

### Project Setup

clice reads a [compilation database](https://clang.llvm.org/docs/JSONCompilationDatabase.html) to understand your project. For CMake:

```shell
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

By default clice loads every `compile_commands.json` in the project root and its immediate subdirectories (e.g. `build/`). See [project setup](https://docs.clice.io/clice/guide/quick-start#project-setup) for other build systems, and the [configuration guide](https://docs.clice.io/clice/guide/config-guide) for databases elsewhere and files without one.

> [!NOTE]
> clice 1.0 is its first stable release. Bug reports via [issues](https://github.com/clice-io/clice/issues) are welcome.

## Documentation

Full docs at [**docs.clice.io/clice**](https://docs.clice.io/clice/): the guide and configuration, every language feature and command, and the design.

## Contributing

See [building from source](https://docs.clice.io/clice/guide/build) or join our [Discord](https://discord.gg/PA3UxW2VA3).

```shell
bazel build //:dist   # clice and the unit tests
pixi run test         # the unit tests and the cases
```
