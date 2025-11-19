<!-- Begin section: Overview -->

# clice

![C++ Standard](https://img.shields.io/badge/C++-23-blue.svg)
[![GitHub license](https://img.shields.io/github/license/clice-io/clice)](https://github.com/clice-io/clice/blob/main/LICENSE)
[![Actions status](https://github.com/clice-io/clice/workflows/CI/badge.svg)](https://github.com/clice-io/clice/actions)
[![Documentation](https://img.shields.io/badge/view-documentation-blue)](https://clice.io)
[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/clice-io/clice)
[![Discord](https://img.shields.io/badge/Discord-%235865F2.svg?logo=discord&logoColor=white)](https://discord.gg/PA3UxW2VA3)

clice is a next-generation language server designed for modern C++. Through excellent asynchronous task scheduling and intelligent caching, it achieves a lower memory footprint and faster response times.

Beyond performance, clice provides instantiation-aware template processing, supports switching header contexts between different source files (including non-self-contained headers), and offers comprehensive support for C++20 modules, from code completion to go-to-definition. Our goal is to provide C++ developers with a truly fast, precise, and intelligent development companion.

> [!IMPORTANT]
> Support for header contexts and C++20 modules are core features currently under active development. They will be progressively refined in upcoming releases. Stay tuned!

## Getting started

Download the latest `clice` binary from the [releases page](https://github.com/clice-io/clice/releases) ( or  [nightly build](https://nightly.link/clice-io/clice/workflows/nightly/main) ) and install the [vscode extension](https://marketplace.visualstudio.com/items?itemName=ykiko.clice-vscode). Then, add the following configuration to your `.vscode/settings.json` file:

```jsonc
{
    // Optional: Set this to an empty string to turn off the clangd.
    "clangd.path": "",

    // Point this to the clice binary you downloaded.
    "clice.executable": "/path/to/your/clice/executable",
}
```

> [!NOTE]
> As an early version, please do not use it in a production environment. Crashes are expected, and we welcome you to submit issues.

## Documentation

To learn more about building, installing, and configuring clice, or to dive deep into its features and architecture, please visit our official documentation at [**clice.io**](https://clice.io/).
