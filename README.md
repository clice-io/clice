# clice

![C++ Standard](https://img.shields.io/badge/C++-23-blue.svg)
[![GitHub license](https://img.shields.io/github/license/clice-io/clice)](https://github.com/clice-io/clice/blob/main/LICENSE)
[![Actions status](https://github.com/clice-io/clice/workflows/main/badge.svg)](https://github.com/clice-io/clice/actions)
[![Documentation](https://img.shields.io/badge/view-documentation-blue)](https://docs.clice.io/clice/)
[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/clice-io/clice)
[![Discord](https://img.shields.io/badge/Discord-%235865F2.svg?logo=discord&logoColor=white)](https://discord.gg/PA3UxW2VA3)

A C++ language server written from scratch on LLVM/Clang, with selected components ported from [clangd](https://clangd.llvm.org/). clice redesigns key architectural decisions to solve long-standing problems in C++ tooling.

## Why clice?

Some problems in C++ tooling require architectural changes that cannot be easily retrofitted into existing language servers. clice addresses these from the ground up:

**Template intelligence** — Type `vec2[0].` inside a template body and get full completions. clice uses pseudo-instantiation to resolve dependent types through nested typedefs and template specializations without needing concrete type arguments. Types like `std::vector<std::vector<T>>::reference` resolve to `std::vector<T>&`, enabling completion, hover, and go-to-definition inside generic code.

**Compilation context** — A first-class concept in clice. For source files, switch between different compilation commands (e.g. different build configurations). For header files, switch which source file provides the including context (preprocessor state, preceding declarations). This handles non-self-contained headers and context-dependent macros naturally, with automatic context switching as you navigate.

**C++20 named modules** — Parallel module compilation driven by a dependency-aware compile graph with interest-counted cancellation. Pre-compiled module interfaces (BMIs) are cached across editor sessions, so reopening a project skips redundant builds. The LSP layer provides module name completion on `import` statements and module-aware semantic highlighting.

## Getting Started

### Install

Download the latest binary from the [releases page](https://github.com/clice-io/clice/releases), or [build from source](https://docs.clice.io/clice/dev/build).

**Platforms:** Linux (x64, ARM64), macOS (x64, ARM64), Windows (x64, ARM64)

**Release channels:**

| Channel        | Version                           | Where to get it                                                                                                                                             |
| -------------- | --------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Stable**     | even minor, e.g. `v0.2.0`         | [releases page](https://github.com/clice-io/clice/releases) + [VS Code Marketplace](https://marketplace.visualstudio.com/items?itemName=clice-io.clice)     |
| **Nightly**    | odd minor, e.g. `v0.1.2026071902` | [GitHub pre-releases](https://github.com/clice-io/clice/releases) + Marketplace pre-release channel (`Switch to Pre-Release Version` on the extension page) |
| **Per-commit** | untagged                          | artifacts on the [latest green `main` CI runs](https://github.com/clice-io/clice/actions/workflows/main.yml?query=branch%3Amain+is%3Asuccess)               |

The versioning rule is the VS Code Marketplace convention: **even minor = stable, odd minor = pre-release**. Nightly patch numbers encode the UTC build time as `YYYYMMDDHH` (year, month, day, hour — e.g. `2026071902` is 2026-07-19 02:00 UTC), so newer is always higher; a nightly is published only when `main` gained commits, and nightlies older than 30 days are removed. Every published binary is the exact one that passed the full test suites — releases promote CI builds rather than rebuilding.

For the freshest bits, every green [`main` CI run](https://github.com/clice-io/clice/actions/workflows/main.yml?query=branch%3Amain+is%3Asuccess) attaches installable artifacts (binaries and platform vsix) to its run page, so a fix can be tried the moment it merges (GitHub login required for artifact downloads).

Each release also ships `*.symbols` packages: if clice crashes, attach the newest log from your workspace's `.clice/logs/` to an issue and the matching symbols let us reconstruct the exact stack.

### Editor Setup

| Editor      | Setup                                                                                                                  |
| ----------- | ---------------------------------------------------------------------------------------------------------------------- |
| **VS Code** | Install the [clice extension](https://marketplace.visualstudio.com/items?itemName=clice-io.clice) from the Marketplace |
| **Neovim**  | Add `editors/nvim` to your runtime path: `vim.opt.rtp:append("/path/to/clice/editors/nvim")`                           |
| **Zed**     | Load `editors/zed` as a local extension                                                                                |
| **Other**   | Any LSP client works — point it at `clice serve`                                                                       |

### Project Setup

clice reads a [compilation database](https://clang.llvm.org/docs/JSONCompilationDatabase.html) to understand your project. For CMake:

```shell
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

By default clice searches the workspace root and its immediate subdirectories (e.g. `build/`) for `compile_commands.json`. See [configuration](https://docs.clice.io/clice/guide/configuration) for custom paths and other build systems.

> [!NOTE]
> clice is in beta on the road to its first stable release: nightlies are ready to try today. Bug reports via [issues](https://github.com/clice-io/clice/issues) are welcome.

## Documentation

Full docs at [**docs.clice.io/clice**](https://docs.clice.io/clice/) covering configuration, architecture, and the feature checklist.

## Contributing

See the [contribution guide](https://docs.clice.io/clice/dev/contribution) or join our [Discord](https://discord.gg/PA3UxW2VA3).

```shell
pixi run build   # configure + build
pixi run test    # unit + integration + smoke tests
```
