# 文档链接

从源文件指令到其解析目标文件的可点击链接。

<!-- The checklist sections below are generated from the snapshot fixtures in
     tests/snap/document_links/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture doc headers and run
     `node tools/docs/feature.ts update`. -->

## Include 指令

<!-- BEGIN GENERATED ITEMS: Include Directives -->

- [x] 引号 include — `#include "..."` 链接到解析后的头文件

  文件中的每个 include 都会链接，而不只是顶部 preamble 运行到的那些。

  <details>
  <summary>示例</summary>

  ```cpp
  #include "header_a.h"
  #include "header_b.h"
  int x = 1;
  #include "header_c.h"
  ```

  </details>

- [x] 尖括号 include — `#include <...>` 链接到搜索路径上找到的头文件

  <details>
  <summary>示例</summary>

  ```cpp
  #include <header_a.h>
  ```

  </details>

- [x] 宏展开路径 — `#include MACRO` 将指令参数链接到展开后的目标（[clangd#2375](https://github.com/clangd/clangd/issues/2375)）

  <details>
  <summary>示例</summary>

  ```cpp
  #define HEADER "header_b.h"
  #include HEADER
  ```

  </details>

- [ ] `#include_next` 和 `__has_include_next` — 链接继续沿搜索路径向下 _（部分）_

  在搜索路径上，`first/wrap.h` 会遮蔽 `second/wrap.h`；其中的
  `#include_next`（由 `__has_include_next` 保护）会包含第二份副本。
  仅当该头文件在包含它的 TU 上下文中编译时，才存在搜索路径中的下一个解析；
  单独打开时，它会被编译为自身的 TU，此时 clang 故意将 `#include_next`
  当作普通 include 处理，因此目前两个链接都会落回第一份副本（这也是
  snapshot 固定的行为）。

  <details>
  <summary>示例</summary>

  `main.cpp`:

  ```cpp
  #include <wrap.h>

  int use_wrap = WRAP_FIRST + WRAP_SECOND;
  ```

  `first/wrap.h`:

  ```cpp
  #pragma once

  #define WRAP_FIRST 1

  #if __has_include_next(<wrap.h>)
  #include_next <wrap.h>
  #endif
  ```

  `second/wrap.h`:

  ```cpp
  #pragma once

  #define WRAP_SECOND 2
  ```

  </details>

- [x] `__has_include` — 检查的路径链接到它探测的文件

  <details>
  <summary>示例</summary>

  ```cpp
  #if __has_include("header_c.h")
  #include "header_c.h"
  #endif
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Embed 指令

<!-- BEGIN GENERATED ITEMS: Embed Directives -->

- [x] `#embed` — 资源路径链接到嵌入的文件

  <details>
  <summary>示例</summary>

  ```cpp
  const char data[] = {
  #embed "data.bin"
  };
  ```

  </details>

- [x] `__has_embed` — 检查的路径链接到探测的资源

  <details>
  <summary>示例</summary>

  ```cpp
  #if __has_embed("data.bin")
  const char first_byte[] = {
  #embed "data.bin" limit(1)
  };
  #endif
  ```

  </details>

<!-- END GENERATED ITEMS -->

## 呈现

<!-- BEGIN GENERATED ITEMS: Presentation -->

- [x] 解析路径 tooltip — 每个链接都将其目标的绝对路径作为 hover tooltip 携带

  编辑器会在跟随链接提示旁边渲染该 tooltip，例如
  `/usr/include/c++/14/vector (ctrl + click)`。Snapshot 只固定链接目标；
  测试套件改为在本语料库中每个 fixture 的服务器回复上，针对目标验证 tooltip。

  <details>
  <summary>示例</summary>

  ```cpp
  #include "header_a.h"
  ```

  </details>

<!-- END GENERATED ITEMS -->

## Module 声明

<!-- BEGIN GENERATED ITEMS: Module Declarations -->

- [ ] Module 目标 — `import` 和 `module` 声明链接到它们的接口文件

  <details>
  <summary>示例</summary>

  ```cpp
  export module app;

  import lib;
  import :part;
  export import lib.extra;
  ```

  </details>

<!-- END GENERATED ITEMS -->
