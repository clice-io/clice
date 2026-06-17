# 文档链接

从源文件指令到其解析目标文件的可点击链接。

## Include 指令

- [x] `#include "..."` — 链接到解析后的头文件
- [x] `#include <...>` — 链接到解析后的系统头文件
- [x] `__has_include(...)` — 链接到检查的文件
- [x] `#embed "..."` — 链接到嵌入资源文件
- [x] `__has_embed(...)` — 链接到检查的嵌入文件
- [x] `#include_next` — 链接到搜索路径中下一个解析的头文件
- [x] 宏展开的 include 路径 — 路径由宏产生时也应解析并链接（[clangd#2375](https://github.com/clangd/clangd/issues/2375)）

  ```cpp
  #define HEADER "config.h"
  #include HEADER  // 应链接到 config.h
  ```

- [x] `__has_include_next(...)` — 链接到检查的文件
- [ ] 在 tooltip 中显示解析后的绝对路径

  ```
  #include <vector>
  // tooltip: /usr/include/c++/14/vector
  ```

## Module 声明

- [ ] `import module_name;` — 链接到模块接口文件
- [ ] `import :partition;` — 链接到 partition 文件
- [ ] `module module_name;` — 链接到模块接口（从实现单元）
- [ ] `export import module_name;` — 链接到重导出的模块接口

## 变更记录

| 日期 | 变更                                                  | PR  |
| ---- | ----------------------------------------------------- | --- |
| —    | Include 指令链接（#include、\_\_has_include、#embed） | —   |
