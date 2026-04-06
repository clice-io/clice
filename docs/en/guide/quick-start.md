# Quick Start

## Editor Plugins

clice implements the [Language Server Protocol](https://microsoft.github.io/language-server-protocol). Any editor that supports this protocol can theoretically work with clice to provide features like `code completion`, `diagnostics`, `go-to-definition`, and more.

However, beyond the standard protocol, clice also supports some protocol extensions. For better handling of these protocol extensions and better integration with editors, using clice plugins in specific editors is often a better choice. Most of them work out of the box and support clice's protocol extensions.

### Visual Studio Code

### Vim/Neovim

### Others

Other editors don't have available clice plugins yet (contributions welcome!). To use clice in them, please install clice yourself and refer to the specific editor's documentation on how to use a language server.

## Installation

If your editor plugin handles clice's download, you can skip this step.

### Download Prebuilt Binary

Download clice binary version through the Release page.

### Build from Source

Build clice from source yourself. For specific steps, refer to [build](../dev/build.md).

## Project Setup

For clice to correctly understand your code (e.g., find header file locations), you need to provide clice with a `compile_commands.json` file, also known as a [compilation database](https://clang.llvm.org/docs/JSONCompilationDatabase.html). The compilation database provides compilation options for each source file.

### CMake

For build systems using cmake, add the `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` option during build, for example:

```cmake
cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

This will generate a `compile_commands.json` file in the `build` directory.

::: warning
Note: This option only works when cmake's generator is set to makefile and ninja. For other generators, this option will be ignored, meaning the compilation database cannot be generated.
:::

### Bazel

Bazel has no native support to generate a compilation database. The recommended solution is to use [bazel-compile-commands-extractor](https://github.com/hedronvision/bazel-compile-commands-extractor). After setting it up, you can generate `compile_commands.json` with:

```bash
bazel run @hedron_compile_commands//:refresh_all
```

### Visual Studio

Visual Studio (2019 16.1+) can generate a compilation database via CMake integration. Open your project as a CMake project, then configure the generation in `CMakeSettings.json`:

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

Alternatively, for MSBuild-based projects (`.vcxproj`), you can use [compiledb-vs](https://github.com/pjbroad/compiledb-vs) or [catter](https://github.com/clice-io/catter) to generate the compilation database.

### Makefile

For Makefile-based projects, use [bear](https://github.com/rizsotto/Bear) to intercept compilation commands:

```bash
bear -- make
```

This will generate a `compile_commands.json` in the current directory. Note that `bear` requires a clean build to capture all commands — run `make clean` before `bear -- make` if needed.

Alternatively, if you use GNU Make, you can use [compiledb](https://github.com/nicktimko/compiledb):

```bash
compiledb make
```

### Meson

Meson generates a compilation database automatically during setup:

```bash
meson setup build
```

The `compile_commands.json` will be in the `build` directory.

### Xmake

Use one of the following approaches to generate a compilation database.

#### Command Line

Run the following command to manually generate a compilation database:

```bash
xmake project -k compile_commands --lsp=clangd build
```

> Compilation database generated manually doesn't automatically update itself. Re-generate if changes are made to the project.

#### VSCode Extension

The Xmake official VSCode extension automatically generates the compilation database when `xmake.lua` is updated. However, it generates the database to the `.vscode` directory by default. Add this setting in `settings.json`:

```json
"xmake.compileCommandsDirectory": "build"
```

to explicitly ask the extension to generate the compilation database in `build`.

### Others

For any other build system, you can try the following tools to intercept compilation commands and generate a compilation database:

- [bear](https://github.com/rizsotto/Bear) — intercepts `exec` calls to capture compilation commands. Works with most build systems on Linux/macOS.
- [scan-build](https://github.com/rizsotto/scan-build) — a Clang tool that can generate compilation databases as a side effect of static analysis.
- [catter](https://github.com/clice-io/catter) — a tool we are developing that captures compilation commands through a fake compiler approach. It is designed to work reliably with any build system that invokes a compiler executable.
