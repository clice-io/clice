# 文档链接

从源码指令跳往解析后目标文件的可点击链接。

`import` 中的模块名不是链接：在它上面执行“跳转到定义”会打开声明该模块的接口单元，参见[模块导航](./navigation.md#module-navigation)。

<!-- The cards in the sections below are generated from the documented cases
     in tests/document_links/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run docs-update`. -->

## 包含指令

`#include` 的头文件名与 `__has_include` 检查的路径，都会链接到解析所得的文件。

<!-- BEGIN GENERATED ITEMS: document_links/include_directives -->

<!-- BEGIN CAPABILITY: supported -->

**双引号与尖括号包含指令**

每条包含指令都将头文件名链接到解析所得的文件

```case
tests/document_links/include_directives/01_header_paths
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported clangd#2375 -->

**宏展开路径**

`#include MACRO` 将宏名链接到宏展开后指向的头文件

```case
tests/document_links/include_directives/03_macro_include
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: partial -->

**`#include_next`**

`#include_next` 和 `__has_include_next` 链接到搜索路径上的下一个同名头文件

此处 `first/wrap.h` 遮蔽了 `second/wrap.h`，前者的 `#include_next` 会找到第二份头文件。

```case
tests/document_links/include_directives/04_include_next
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`__has_include`**

所检查的路径链接到其查找的文件

```case
tests/document_links/include_directives/05_has_include
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 嵌入指令

`#embed` 指定的资源与 `__has_embed` 检查的路径，都会链接到其指明的文件。

<!-- BEGIN GENERATED ITEMS: document_links/embed_directives -->

<!-- BEGIN CAPABILITY: supported -->

**`#embed`**

资源路径链接到所嵌入的文件

```case
tests/document_links/embed_directives/01_embed
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**`__has_embed`**

所检查的路径链接到其查找的资源

```case
tests/document_links/embed_directives/02_has_embed
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
