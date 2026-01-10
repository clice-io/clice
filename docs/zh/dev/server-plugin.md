你可以在 clice 中实现一个 server plugin 来扩展 clice 的功能。

## 用例

当你使用 `clice` 作为 LLM 代理的 LSP 后端时，比如 claude code，你可以添加插件来提供一些额外功能。

## 编写一个插件

当一个插件被服务器加载时，它会调用 `clice_get_server_plugin_info` 来获取关于这个插件的信息以及如何注册它的定制点。

这个函数需要由插件实现，请参考下面的示例：

```cpp
extern "C" ::clice::PluginInfo LLVM_ATTRIBUTE_WEAK
clice_get_server_plugin_info() {
  return {
    CLICE_PLUGIN_API_VERSION, "MyPlugin", "v0.1", CLICE_PLUGIN_DEF_HASH,
    [](ServerPluginBuilder builder) {  ... }
  };
}
```

请参考 [PluginDef.h](/include/Server/PluginDef.h) 了解更多细节。

## 获取 `CLICE_PLUGIN_DEF_HASH` 的内容

在 `clice_get_server_plugin_info` 函数中需要返回两个值。

- `CLICE_PLUGIN_API_VERSION` 用于确保插件和服务器之间的 `clice_get_server_plugin_info` 函数的一致性。
- `CLICE_PLUGIN_DEF_HASH` 用于确保插件和服务器之间的 C++ 声明的一致性。

要调试 `CLICE_PLUGIN_DEF_HASH` 的内容，你可以运行以下命令：

```shell
$ git clone https://github.com/clice-io/clice.git
$ cd clice
$ git checkout `clice --version --git-describe`
$ python scripts/plugin-def.py content > /tmp/plugin-proto.h
```

你将会得到一个 C 源码格式的文件，内容大致如下：

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
