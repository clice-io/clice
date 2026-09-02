# 文档链接

从源代码指令到解析所得目标文件的可点击链接。

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/document_links/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture doc headers and run
     `node tools/docs/feature.ts update`. -->

## 包含指令

<!-- BEGIN GENERATED ITEMS: include_directives -->

| 能力                                    | 状态     | 问题                                                        |
| --------------------------------------- | -------- | ----------------------------------------------------------- |
| 引号形式的包含指令                      | 支持     |                                                             |
| 尖括号形式的包含指令                    | 支持     |                                                             |
| 宏展开路径                              | 支持     | [clangd#2375](https://github.com/clangd/clangd/issues/2375) |
| `#include_next` 和 `__has_include_next` | 部分支持 |                                                             |
| `__has_include`                         | 支持     |                                                             |

### 引号形式的包含指令

`#include "..."` 链接到解析出的头文件

文件中的每条包含指令都会建立链接，而不仅限于顶部 Preamble 处理过的包含指令。

```cpp
#include "header_a.h"
#include "header_b.h"
int x = 1;
#include "header_c.h"
```

### 尖括号形式的包含指令

`#include <...>` 链接到在搜索路径上找到的头文件

```cpp
#include <header_a.h>
```

### 宏展开路径

`#include MACRO` 将指令参数链接到宏展开后的目标

```cpp
#define HEADER "header_b.h"
#include HEADER
```

### `#include_next` 和 `__has_include_next`

链接会继续沿搜索路径向下查找

在搜索路径上，`first/wrap.h` 会遮蔽 `second/wrap.h`；其中的
`#include_next`（由 `__has_include_next` 保护）会包含第二份副本。
只有在包含该头文件的 TU 上下文中编译它时，才能解析到搜索路径中的下一项；
单独打开时，它会作为独立 TU 编译，此时 clang 会有意将 `#include_next`
当作普通包含指令处理，因此目前两个链接都会回到第一份副本（这也是
快照所固定的行为）。

`main.cpp`：

```cpp
#include <wrap.h>

int use_wrap = WRAP_FIRST + WRAP_SECOND;
```

`first/wrap.h`：

```cpp
#pragma once

#define WRAP_FIRST 1

#if __has_include_next(<wrap.h>)
#include_next <wrap.h>
#endif
```

`second/wrap.h`：

```cpp
#pragma once

#define WRAP_SECOND 2
```

### `__has_include`

被检查的路径会链接到其所探测的文件

```cpp
#if __has_include("header_c.h")
#include "header_c.h"
#endif
```

<!-- END GENERATED ITEMS -->

## 嵌入指令

<!-- BEGIN GENERATED ITEMS: embed_directives -->

| 能力          | 状态 | 问题 |
| ------------- | ---- | ---- |
| `#embed`      | 支持 |      |
| `__has_embed` | 支持 |      |

### `#embed`

资源路径会链接到嵌入的文件

```cpp
const char data[] = {
#embed "data.bin"
};
```

### `__has_embed`

被检查的路径会链接到所探测的资源

```cpp
#if __has_embed("data.bin")
const char first_byte[] = {
#embed "data.bin" limit(1)
};
#endif
```

<!-- END GENERATED ITEMS -->

## 呈现

<!-- BEGIN GENERATED ITEMS: presentation -->

| 能力               | 状态 | 问题 |
| ------------------ | ---- | ---- |
| 已解析路径工具提示 | 支持 |      |

### 已解析路径工具提示

每个链接都以悬停工具提示的形式提供其目标的绝对路径

编辑器会在链接跳转提示旁显示该工具提示，例如
`/usr/include/c++/14/vector (ctrl + click)`。快照仅固定链接目标；
测试套件则针对本测试集中的每个测试样例，在服务器响应中验证工具提示
与目标相符。

```cpp
#include "header_a.h"
```

<!-- END GENERATED ITEMS -->

## 模块声明

<!-- BEGIN GENERATED ITEMS: module_declarations -->

| 能力     | 状态   | 问题 |
| -------- | ------ | ---- |
| 模块目标 | 不支持 |      |

### 模块目标

`import` 和 `module` 声明会链接到相应的接口文件

```cpp
export module app;

import lib;
import :part;
export import lib.extra;
```

<!-- END GENERATED ITEMS -->
