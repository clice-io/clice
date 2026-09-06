# 文档链接

从源代码指令到解析所得目标文件的可点击链接。

<!-- The capability sections below are generated from the snapshot fixtures in
     tests/snap/document_links/. Do not edit the regions between the GENERATED
     markers by hand — edit the fixture doc headers and run
     `node tools/docs/feature.ts update`. -->

## 包含指令

<!-- BEGIN GENERATED ITEMS: include_directives -->

<!-- BEGIN CAPABILITY: supported -->

**使用引号的包含指令**

`#include "..."` 链接到解析得到的头文件

文件中的所有包含指令都有链接，不限于文件顶部 Preamble 中的那些。

```snap
tests/snap/document_links/include_directives/01_quoted_include.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**使用尖括号的包含指令**

`#include <...>` 链接到在搜索路径中找到的头文件

```snap
tests/snap/document_links/include_directives/02_angle_include.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2375 -->

**宏展开后的路径**

`#include MACRO` 将指令参数链接到宏展开后的目标文件

```snap
tests/snap/document_links/include_directives/03_macro_include.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**`#include_next` 和 `__has_include_next`**

单独打开头文件时，`#include_next` 会链接回第一个匹配的头文件

在搜索路径中，`first/wrap.h` 遮蔽了 `second/wrap.h`；前者通过 `#include_next`（由 `__has_include_next` 检查把关）包含第二个同名头文件。只有在包含该头文件的 TU 上下文中编译它时，才会沿搜索路径解析到下一个匹配项。单独打开时，该头文件会作为独立的 TU 编译，此时 `#include_next` 的行为与普通包含指令相同，两个链接都会指回第一个同名头文件。

```snap
tests/snap/document_links/include_directives/04_include_next/main.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`__has_include`**

被检查的路径链接到所探测的文件

```snap
tests/snap/document_links/include_directives/05_has_include.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 嵌入指令

<!-- BEGIN GENERATED ITEMS: embed_directives -->

<!-- BEGIN CAPABILITY: supported -->

**`#embed`**

资源路径链接到嵌入的文件

```snap
tests/snap/document_links/embed_directives/01_embed.cpp
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`__has_embed`**

被检查的路径链接到所探测的资源

```snap
tests/snap/document_links/embed_directives/02_has_embed.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 呈现

<!-- BEGIN GENERATED ITEMS: presentation -->

<!-- BEGIN CAPABILITY: supported -->

**解析后路径的工具提示**

每个链接都以目标文件的绝对路径作为悬停时的工具提示

编辑器会在打开链接的操作提示旁显示工具提示，例如 `/usr/include/c++/14/vector (ctrl + click)`。

```snap
tests/snap/document_links/presentation/01_tooltip.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块声明

<!-- BEGIN GENERATED ITEMS: module_declarations -->

<!-- BEGIN CAPABILITY: unsupported -->

**模块目标**

`import` 和 `module` 声明尚未链接到接口文件

```snap
tests/snap/document_links/module_declarations/01_modules.cpp
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
