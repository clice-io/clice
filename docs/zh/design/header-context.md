# Header Context

## 问题

编译数据库（`compile_commands.json`）只包含源文件的条目，不包含头文件。由于头文件是被包含到源文件中的，它们的含义会随包含上下文的不同而变化。

### 上下文相关的头文件

```cpp
// a.h
#ifdef TEST
struct X { ... };
#else
struct Y { ... };
#endif

// b.cpp
#define TEST
#include "a.h"

// c.cpp
#include "a.h"
```

`a.h` 在 `b.cpp`（定义 `X`）和 `c.cpp`（定义 `Y`）中具有不同的状态。将 `a.h` 当做独立源文件处理的语言服务器只能显示其中一种状态。

### 非自包含头文件

```cpp
// a.h
struct Y {
    X x;
};

// b.cpp
struct X {};
#include "a.h"
```

`a.h` 自身无法编译，但嵌入到 `b.cpp` 中就能正常编译。clangd 在这类头文件中会报错，因为它将头文件作为独立源文件编译。这种模式在 libstdc++ 和流行的 header-only 库中很常见。

## clice 的方案

clice 实现了头文件上下文切换——当你跳转到一个头文件时，它会使用你跳转来源的源文件作为编译上下文。

### 工作原理

1. **上下文解析**（`Compiler::resolve_header_context`）：当头文件被打开时，clice 通过 `DependencyGraph::find_host_sources` 找到包含它的宿主源文件，构建从宿主源文件到目标头文件的包含链。

2. **Preamble 注入**：宿主源文件中 `#include` 目标头文件之前的所有内容被写入一个临时 preamble 文件。然后在头文件的编译标志中注入 `-include <preamble>`。这使头文件获得与从该源文件包含时完全一致的预处理器状态。

3. **自动切换**：当你从源文件跳转到头文件（例如通过跳转到定义或文件打开），clice 自动使用该源文件的上下文。上下文按会话存储在 `HeaderFileContext` 中。

4. **手动切换**：用户可以通过 `clice/switchContext` 命令显式切换上下文，该命令会列出当前头文件的所有可用宿主源文件。

### 上下文失效

当宿主源文件的 preamble 发生变化（include 顺序修改、宏添加）时，头文件上下文会失效。preamble 的内容哈希决定新鲜度——如果哈希变化，下次查询时会重建上下文。
