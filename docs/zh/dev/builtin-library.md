# Builtin Libraries

Builtin library 会被直接编译进 `clice` 可执行文件，而不是在运行时通过 `--plugin-path` 动态加载。

这种方式适合以下场景：

- 插件源码位于 `clice` 源码树之外
- 希望该 builtin 默认随 `clice` 可执行文件一起发布
- 需要为该 builtin 单独补充 include 路径、编译宏或链接依赖

## CMake 入口

`clice` 现在提供了一个辅助模块：[cmake/builtin-libraries.cmake](/cmake/builtin-libraries.cmake)。

额外的 builtin library 通过缓存变量 `CLICE_BUILTIN_LIBRARY_MODULES` 注册。

`CLICE_BUILTIN_LIBRARY_MODULES` 中的每一项都必须是一个 CMake 文件。配置阶段 `clice` 会 `include()` 这些文件，而每个文件都需要调用 `clice_add_builtin_library(...)`。

## 最小示例

你可以在外部项目中创建一个 CMake 文件，例如 `/path/to/my-plugin/clice-builtin.cmake`：

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

然后在配置 `clice` 时传入：

```shell
cmake -B build -G Ninja \
    -DCLICE_BUILTIN_LIBRARY_MODULES="/path/to/my-plugin/clice-builtin.cmake"
```

如果要加载多个模块，可以传入以分号分隔的 CMake 列表：

```shell
cmake -B build -G Ninja \
    -DCLICE_BUILTIN_LIBRARY_MODULES="/path/to/a.cmake;/path/to/b.cmake"
```

## `clice_add_builtin_library`

该辅助函数支持以下参数：

| 参数 | 必填 | 说明 |
| --- | --- | --- |
| `NAME` | 是 | 逻辑名称，会用于创建内部 object target |
| `SOURCES` | 是 | 要编译进 `clice` 的源文件，支持绝对路径和源码树外路径 |
| `ENTRYPOINT` | 是 | `clice` 命名空间中的唯一函数名，返回值类型为 `::clice::PluginInfo` |
| `INCLUDE_DIRECTORIES` | 否 | 仅对当前 builtin 生效的额外头文件目录 |
| `LINK_LIBRARIES` | 否 | 当前 builtin 额外需要的库或 target |
| `COMPILE_DEFINITIONS` | 否 | 当前 builtin 额外需要的编译宏 |
| `COMPILE_OPTIONS` | 否 | 当前 builtin 额外需要的编译选项 |

## Entrypoint 要求

所有 builtin library 最终都会被链接进同一个可执行文件，因此每个 builtin 都必须在 `clice` 命名空间中使用唯一的入口函数名。

动态插件通常使用：

```cpp
clice_get_server_plugin_info()
```

builtin library 应该改用类似下面的唯一名字：

```cpp
clice_get_my_plugin_server_plugin_info()
```

例如：

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
            // 在这里注册回调
        },
    };
}

}  // namespace clice
```

`clice` 会自动生成静态注册代码，因此只要模块被包含进来，就不需要再手动修改 `src/clice.cc`。
