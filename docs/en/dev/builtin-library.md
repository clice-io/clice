# Builtin Libraries

Builtin libraries are compiled directly into the `clice` binary instead of being loaded later with `--plugin-path`.

This is useful when:

- your plugin sources live outside the `clice` source tree
- you want the builtin to be part of the default executable
- you need extra include directories, compile definitions, or link dependencies during the main build

## CMake Entry Point

`clice` now exposes a small helper module at [cmake/builtin-libraries.cmake](/cmake/builtin-libraries.cmake).

Extra builtin libraries are registered through the cache variable `CLICE_BUILTIN_LIBRARY_MODULES`.

Each value in `CLICE_BUILTIN_LIBRARY_MODULES` must be a CMake file. During configure, `clice` includes those files, and each file calls `clice_add_builtin_library(...)`.

## Minimal Module

Create a CMake file in your external project, for example `/path/to/my-plugin/clice-builtin.cmake`:

```cmake
clice_add_builtin_library(
    NAME my_plugin
    SOURCES
        "${CMAKE_CURRENT_LIST_DIR}/src/MyPlugin.cpp"
    INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_LIST_DIR}/include"
    ENTRYPOINT
        clice_get_my_plugin_server_plugin_info
)
```

Then configure `clice` with:

```shell
cmake -B build -G Ninja \
    -DCLICE_BUILTIN_LIBRARY_MODULES="/path/to/my-plugin/clice-builtin.cmake"
```

To load multiple modules, pass a semicolon-separated CMake list:

```shell
cmake -B build -G Ninja \
    -DCLICE_BUILTIN_LIBRARY_MODULES="/path/to/a.cmake;/path/to/b.cmake"
```

## `clice_add_builtin_library`

The helper accepts the following arguments:

| Argument | Required | Description |
| --- | --- | --- |
| `NAME` | Yes | Logical name used to create an internal object target |
| `SOURCES` | Yes | Source files compiled into `clice`; absolute paths and out-of-tree paths are supported |
| `ENTRYPOINT` | Yes | Unique function name in namespace `clice` that returns `::clice::PluginInfo` |
| `INCLUDE_DIRECTORIES` | No | Extra include directories for this builtin only |
| `LINK_LIBRARIES` | No | Extra libraries or targets needed by this builtin |
| `COMPILE_DEFINITIONS` | No | Extra compile definitions for this builtin |
| `COMPILE_OPTIONS` | No | Extra compile options for this builtin |

## Entrypoint Requirements

Builtin libraries share a single final executable, so each builtin must use its own unique entrypoint function inside namespace `clice`.

Dynamic plugins use:

```cpp
clice_get_server_plugin_info()
```

Builtin libraries should use a unique name such as:

```cpp
clice_get_my_plugin_server_plugin_info()
```

For example:

```cpp
#include "Server/Plugin.h"

namespace clice {

::clice::PluginInfo clice_get_my_plugin_server_plugin_info() {
    return {
        CLICE_PLUGIN_API_VERSION,
        "MyPlugin",
        "v0.0.1",
        CLICE_PLUGIN_DEF_HASH,
        [](clice::ServerPluginBuilder& builder) {
            // register callbacks here
        },
    };
}

}  // namespace clice
```

`clice` generates the static registration glue automatically, so once the module is included, no additional edits to `src/clice.cc` are required.
