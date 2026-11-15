# Quick Start

## Editor Plugins

clice implements the [Language Server Protocol](https://microsoft.github.io/language-server-protocol). Any editor that supports this protocol can work with clice to provide features like `code completion`, `diagnostics`, `hover`, `semantic highlighting`, and more.

Beyond the standard protocol, clice also supports some protocol extensions. For better integration, using a dedicated clice plugin is recommended.

See [Editor Setup](./editors.md) for per-editor instructions: official plugins for Visual Studio Code, Neovim and Zed, plus configuration snippets for editors with generic LSP clients (Helix, Emacs, Sublime Text, Kate, Vim).

## Installation

### Download Prebuilt Binary

Download the latest binary from the [Releases](https://github.com/clice-io/clice/releases) page.

### Build from Source

See [build from source](./build.md) for detailed instructions.

## Project Setup

For clice to correctly understand your code (e.g., find header file locations), you need to provide a `compile_commands.json` file, also known as a [compilation database](https://clang.llvm.org/docs/JSONCompilationDatabase.html). The compilation database provides compilation options for each source file.

By default, clice loads every `compile_commands.json` in the project's root directory and its immediate subdirectories (e.g. `build/`), and those in the directories between a file you open and the root, so a database deeper in the tree loads the first time one of its files is opened. A subdirectory with a `clice.toml` or `.clice/config.toml` of its own is a separate project, whose databases the outer one never loads. This discovery turns off as soon as any rule sets `compile_commands` or `default_command`. The [Configuration Guide](./config-guide.md#compile-commands) shows how to name the databases in `clice.toml`, and how to compile files that have no database entry.

### CMake

Add `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` during configuration:

```shell
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

This generates `compile_commands.json` in the `build` directory.

::: warning
This option only works with Makefile and Ninja generators. For other generators (e.g., Visual Studio), use an alternative approach below.
:::

### Bazel

Use the `compdb` module of clice's Bazel registry. Add the registry and the module to the project:

```
# .bazelrc
common --registry=https://bazel.clice.io/
common --registry=https://bcr.bazel.build/
```

```python
# MODULE.bazel
bazel_dep(name = "compdb", version = "0.1.0", dev_dependency = True)
```

Then write `compile_commands.json` beside `MODULE.bazel`:

```bash
bazel run @compdb//:refresh -- --output_groups=+compilation_prerequisites_INTERNAL_
```

It covers `//...`; targets after `--` narrow it. The added output group builds what the commands read but do not build: the generated headers and the `_virtual_includes` directories through which Bazel exposes a library's headers. Without it, those headers are missing until something else builds them, and every file including them reports them not found.

### Visual Studio

For CMake projects in Visual Studio (2019 16.1+), configure in `CMakeSettings.json`:

```json
{
  "configurations": [
    {
      "name": "x64-Debug",
      "generator": "Ninja",
      "buildRoot": "${projectDir}\\build",
      "cmakeCommandArgs": "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
    }
  ]
}
```

For MSBuild projects (`.vcxproj`), use [catter](https://github.com/clice-io/catter) to generate the compilation database.

### Makefile

Use [bear](https://github.com/rizsotto/Bear) to intercept compilation commands:

```bash
bear -- make
```

Run `make clean` before `bear -- make` to ensure all commands are captured.

### Meson

Meson generates a compilation database automatically:

```bash
meson setup build
```

### Xmake

```bash
xmake project -k compile_commands --lsp=clangd build
```

Or configure the Xmake VS Code extension to auto-generate:

```json
"xmake.compileCommandsDirectory": "build"
```

### Others

For any other build system, use [catter](https://github.com/clice-io/catter) — a fake-compiler approach that works with any build system.

## Pre-build the Index (Optional)

Background indexing starts automatically when you open the project in an
editor. For large projects you can also build the index ahead of time from
the command line:

```bash
clice index --workspace /path/to/project
```

The run is resumable — interrupting it with Ctrl-C saves progress, and the
next invocation continues where it left off instead of starting over.
`clice index stats --workspace /path/to/project` prints what the persisted index contains. [A Faster First Index](./config-guide.md#a-faster-first-index) says how to make the first one take less time.
