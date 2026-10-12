# 头文件上下文

头文件的语义取决于包含它的文件中位于 `#include` 之前的宏和声明。clice 会将头文件作为包含它的文件（即宿主文件）的一部分来编译，使头文件的解析结果与构建时一致；如果构建过程以多种方式编译同一个源文件，每条命令就各有一个对应的上下文。编辑器会显示当前上下文，并可切换到其他上下文。[编译上下文](../design/compilation-context.md)介绍了这一设计。

<!-- The cards in the sections below are generated from the documented cases
     in tests/contexts/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## 依赖包含方的头文件

依赖包含方预先给出的声明才能编译的头文件，可以在该包含方的上下文中正常工作。

<!-- BEGIN GENERATED ITEMS: contexts/self_containment -->

<!-- BEGIN CAPABILITY: supported -->

**依赖包含方的头文件**

即使头文件用到了包含方预先给出的声明，也能在包含方的上下文中编译

`ruler.h` 使用了 `main.cpp` 在包含它之前声明的类型和宏，因此无法单独编译。clice 会识别出这一点，并在处理完 `main.cpp` 中的这些声明后编译该头文件，因此头文件内不会报错，悬停功能也能正常工作。

```case
tests/contexts/self_containment/01_host_declarations
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 选择宿主文件

当头文件被多个文件包含时，其解析结果取决于所选的包含方，编辑器可以切换到另一个包含方。

<!-- BEGIN GENERATED ITEMS: contexts/switching -->

<!-- BEGIN CAPABILITY: supported -->

**选择头文件的宿主文件**

依赖包含方的头文件，其解析结果取决于所选的包含方

`shared.h` 使用了各个源文件在包含它之前定义的宏。两个源文件都提供了该头文件的上下文：clice 会选择其中一个，编辑器可以切换到另一个，悬停随即显示由该宿主文件决定的类型。重启后仍会保留这一选择，直到将其重置。

```case
tests/contexts/switching/01_two_hosts
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 包含的代码片段

在声明内部（例如类体或枚举项列表中）包含的文件，会在该位置编译。

<!-- BEGIN GENERATED ITEMS: contexts/fragments -->

<!-- BEGIN CAPABILITY: supported -->

**类体片段**

在类体内包含的文件会作为该类的一部分编译

`body.inc` 中存放着一些成员，`rec.cpp` 将其包含在 `Record` 的大括号之间。单独打开 `body.inc` 时，它仍会在类内部编译，因此不会报错，成员也会列出，并且可以对这些成员使用悬停和跳转到定义功能。

```case
tests/contexts/fragments/01_class_body
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**X-macro 列表**

宏调用列表会在包含方首次展开它的位置编译

`errors.def` 是一个 X-macro 列表：`codes.cpp` 包含它两次，一次用于声明枚举项，另一次用于为它们提供名称。单独打开该文件时，它会在枚举内部编译，因此其中的条目会显示为枚举项。

```case
tests/contexts/fragments/02_enumerator_list
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 以多种方式构建的文件

如果构建时用多条命令编译同一个源文件，每条命令都会对应一个上下文。

<!-- BEGIN GENERATED ITEMS: contexts/hosts -->

<!-- BEGIN CAPABILITY: supported -->

**以多种方式构建的文件**

如果编译数据库为同一个源文件列出了多条命令，每条命令都可作为一个上下文

当构建过程多次编译同一个文件时，例如在每种配置下各编译一次，每条命令都对应一个以其编译选项标识的上下文，编辑器可以在这些上下文之间切换。

```case
tests/contexts/hosts/02_multiple_entries
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
