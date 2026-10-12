# 文档链接

从源代码指令到解析所得目标文件的可点击链接。

<!-- The cards in the sections below are generated from the documented cases
     in tests-next/document_links/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## 包含指令

`#include` 中的头文件名和 `__has_include` 检查的路径，都链接到解析后对应的文件。

<!-- BEGIN GENERATED ITEMS: document_links/include_directives -->

<!-- BEGIN CAPABILITY: supported -->

**使用引号和尖括号的包含指令**

每条包含指令都将头文件名链接到解析后对应的文件

```case
tests-next/document_links/include_directives/01_header_paths
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2375 -->

**宏展开后的路径**

`#include MACRO` 将宏名链接到宏展开后所指向的头文件

```case
tests-next/document_links/include_directives/03_macro_include
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**`#include_next`**

`#include_next` 和 `__has_include_next` 链接到搜索路径中的下一个同名头文件

这里，`first/wrap.h` 遮蔽了 `second/wrap.h`，前者的 `#include_next` 会找到后者。

```case
tests-next/document_links/include_directives/04_include_next
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`__has_include`**

被检查的路径链接到所探测的文件

```case
tests-next/document_links/include_directives/05_has_include
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 嵌入指令

`#embed` 指定的资源和 `__has_embed` 检查的路径，都链接到它们所指的文件。

<!-- BEGIN GENERATED ITEMS: document_links/embed_directives -->

<!-- BEGIN CAPABILITY: supported -->

**`#embed`**

资源路径链接到嵌入的文件

```case
tests-next/document_links/embed_directives/01_embed
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`__has_embed`**

被检查的路径链接到所探测的资源

```case
tests-next/document_links/embed_directives/02_has_embed
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块声明

`import` 中的模块名链接到声明该模块的文件。

<!-- BEGIN GENERATED ITEMS: document_links/module_declarations -->

<!-- BEGIN CAPABILITY: supported -->

**模块目标**

每个导入的模块名都链接到声明该模块的文件

```case
tests-next/document_links/module_declarations/01_module_imports
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 呈现

将鼠标悬停在链接上时，会显示目标文件的绝对路径。

<!-- BEGIN GENERATED ITEMS: document_links/presentation -->

<!-- BEGIN CAPABILITY: supported -->

**解析后路径的工具提示**

每个链接都以目标文件的绝对路径作为悬停时的工具提示

编辑器会在链接跳转提示旁显示工具提示，例如
`/usr/include/c++/14/vector (ctrl + click)`。

```case
tests-next/document_links/presentation/01_tooltips
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
