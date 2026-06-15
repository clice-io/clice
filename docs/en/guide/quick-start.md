# Quick Start

## Editor Plugins

clice implements the [Language Server Protocol](https://microsoft.github.io/language-server-protocol). Any editor that supports this protocol can work with clice to provide features like `code completion`, `diagnostics`, `hover`, `semantic highlighting`, and more.

Beyond the standard protocol, clice also supports some protocol extensions. For better integration, using a dedicated clice plugin is recommended.

### Visual Studio Code

Install the [clice](https://marketplace.visualstudio.com/items?itemName=ykiko.clice-vscode) extension from the Marketplace. It handles downloading the clice binary automatically.

To use a custom binary, set `clice.executable` in your workspace settings:

```jsonc
{
  "clice.executable": "/path/to/clice",
}
```

### Vim/Neovim

Add the clice Neovim plugin to your runtime path:

```lua
-- lazy.nvim
{ dir = "/path/to/clice/editors/nvim" }

-- or manually
vim.opt.rtp:append("/path/to/clice/editors/nvim")
```

### Others

Other editors don't have dedicated clice plugins yet (contributions welcome!). To use clice in them, install the binary and configure your editor's LSP client to run `clice server`.

## Installation

### Download Prebuilt Binary

Download the latest binary from the [Releases](https://github.com/clice-io/clice/releases) page.

### Build from Source

See [build from source](../dev/build.md) for detailed instructions.

## Project Setup

For clice to correctly understand your code (e.g., find header file locations), you need to provide a `compile_commands.json` file, also known as a [compilation database](https://clang.llvm.org/docs/JSONCompilationDatabase.html). The compilation database provides compilation options for each source file.

By default, clice searches your workspace root and each of its immediate subdirectories (e.g. `build/`) for `compile_commands.json`, using the first one it finds. You can specify exact paths with the `compile_commands_paths` option in [clice.toml](./configuration.md).

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

Use [bazel-compile-commands-extractor](https://github.com/hedronvision/bazel-compile-commands-extractor):

```bash
bazel run @hedron_compile_commands//:refresh_all
```

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
