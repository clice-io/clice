You can implement a clice server plugin to extend clice's functionality.

## Use case

When you use `clice` as lsp backend of LLM agents, e.g. claude code, you can add plugin to provide some extra features.

## Writing a plugin

When a plugin is loaded by the server, it will call `clice_get_server_plugin_info` to obtain information about this plugin and about how to register its customization points.

This function needs to be implemented by the plugin, see the example below:

```c++
extern "C" ::clice::PluginInfo LLVM_ATTRIBUTE_WEAK
clice_get_server_plugin_info() {
  return {
    CLICE_PLUGIN_API_VERSION, "MyPlugin", "v0.1", CLICE_PLUGIN_DEF_HASH,
    [](ServerPluginBuilder builder) {  ... }
  };
}
```

See [PluginDef.h](/include/Server/PluginDef.h) for more details.

## Compiling a plugin

The plugin must be compiled with the same dependencies and compiler options as clice, otherwise it will cause undefined behavior. [config/llvm-manifest.json](/config/llvm-manifest.json) defines the build information used by clice.

## Loading plugins

For security reasons, clice does not allow loading plugins through configuration files, but must specify the plugin path through command line options.

When `clice` starts, it will load all plugins specified in the command line. You can specify the plugin path through the `--plugin-path` option.

```shell
$ clice --plugin-path /path/to/my-plugin.so
```

## Getting content of `CLICE_PLUGIN_DEF_HASH`

There are two values to return in the `clice_get_server_plugin_info` function.

- `CLICE_PLUGIN_API_VERSION` is used to ensure compability of the `clice_get_server_plugin_info` function between the plugin and the server.
- `CLICE_PLUGIN_DEF_HASH` is used to ensure the consistency of the C++ declarations between the plugin and the server.

To debug the content of `CLICE_PLUGIN_DEF_HASH`, you can run following command:

```shell
$ git clone https://github.com/clice-io/clice.git
$ cd clice
$ git checkout `clice --version --git-describe`
$ python scripts/plugin-def.py content
```

You will get a C source code file, content of which is like this:

```cpp
#if 0
// begin of config/llvm-manifest.json
[
  {
    "version": "21.1.4+r1",
    "filename": "arm64-macos-clang-debug-asan.tar.xz",
    "sha256": "7da4b7d63edefecaf11773e7e701c575140d1a07329bbbb038673b6ee4516ff5",
    "lto": false,
    "asan": true,
    "platform": "macosx",
    "build_type": "Debug"
  },
  ...
]
...
#endif
```
