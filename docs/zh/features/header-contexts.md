# 头文件上下文

头文件的语义取决于包含它的文件中位于 `#include` 之前的宏和声明。clice 会将头文件作为包含它的文件（即宿主文件）的一部分来编译，使头文件的解析结果与构建时一致；如果构建过程以多种方式编译同一个源文件，每条命令就各有一个对应的上下文。编辑器会显示当前活动的上下文，并可切换到其他上下文。[编译上下文](../design/compilation-context.md)介绍了这一设计。

<!-- The cards in the sections below are generated from the documented cases
     in tests/contexts/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run docs-update`. -->

## 依赖包含方的头文件

必须先有包含方的声明才能编译的头文件，可以在该包含方的上下文中正常工作。

<!-- BEGIN GENERATED ITEMS: contexts/self_containment -->

<!-- BEGIN CAPABILITY: supported -->

**依赖包含方的头文件**

用到了包含方前置声明的头文件，仍可在包含方的上下文中正常编译

`ruler.h` 使用了 `main.cpp` 在包含它之前声明的类型和宏，因此无法单独编译。clice 会识别出这一点，并在 `main.cpp` 的这些声明之后编译该头文件，因此头文件内不会报错，悬停也能正常工作。

```case
tests/contexts/self_containment/01_host_declarations
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 选择宿主文件

有多个包含方的头文件按当前所选包含方的方式解析，编辑器也可以切换到另一个包含方。

<!-- BEGIN GENERATED ITEMS: contexts/switching -->

<!-- BEGIN CAPABILITY: supported -->

**选择头文件的宿主文件**

依赖包含方的头文件按当前所选包含方的方式解析

`shared.h` 使用了各个源文件在包含它之前定义的宏。两个源文件都是该头文件的上下文：clice 会选择其中一个，编辑器可以切换到另一个，悬停随即显示该宿主文件决定的类型。这一选择在重启后仍会保留，直至重置。

```case
tests/contexts/switching/01_two_hosts
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**其他宿主文件编译的代码**

头文件中仅由其他宿主文件编译的代码，仍保留语义着色、大纲、折叠范围、文档链接和悬停

`shared.h` 在被 `fast_host.cpp` 包含时定义 `fast_path()`，在被 `slow_host.cpp` 包含时定义 `slow_path()`。在第一个宿主文件的上下文中查看时，第二个分支会作为非活动代码变暗显示，但仍保留语义着色、大纲条目、折叠范围和包含链接，悬停在 `slow_path` 上也能显示其声明：clice 的索引保存了该头文件的所有上下文，只要文件内容与磁盘保存状态一致，各项响应就会从索引中补齐其他上下文的代码。在切换上下文的菜单中选择 Hide other contexts，即可仅显示第一个宿主文件的上下文。

```case
tests/contexts/switching/14_other_host_code
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 包含的代码片段

在声明内部（例如类体或枚举项列表中间）包含的文件，会在其被包含的位置编译。

<!-- BEGIN GENERATED ITEMS: contexts/fragments -->

<!-- BEGIN CAPABILITY: supported -->

**类体片段**

在类体内包含的文件作为该类的一部分编译

`body.inc` 存放了一些类成员，由 `rec.cpp` 包含在 `Record` 的花括号之间。单独打开 `body.inc` 时，它会在该类内部编译，因此不会报错，能列出其中的成员，悬停和跳转到定义也都能正常工作；代码补全还会提供该类在此文件之后声明的成员。

```case
tests/contexts/fragments/01_class_body
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**X-macro 列表**

宏调用列表会在包含方首次展开它的位置编译

`errors.def` 是一个 X-macro 列表：`codes.cpp` 包含它两次，一次用于声明枚举项，一次用于给出它们的名称。单独打开该文件时，它会在枚举内部编译，其中的条目会显示为枚举项。

```case
tests/contexts/fragments/02_enumerator_list
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 以多种方式构建的文件

构建过程使用多条命令编译同一个源文件时，每条命令各对应一个上下文。

<!-- BEGIN GENERATED ITEMS: contexts/hosts -->

<!-- BEGIN CAPABILITY: supported -->

**以多种方式构建的文件**

编译数据库中列有多条命令的源文件会将每条命令作为一个上下文提供

当构建过程多次编译同一个文件时（例如每种构建配置各编译一次），每条命令都是一个可在编辑器中切换的上下文，并以各自的编译选项作为标签。

```case
tests/contexts/hosts/02_multiple_entries
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
