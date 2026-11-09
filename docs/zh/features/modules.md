# C++20 模块

clice 会在文件打开时构建该文件导入的模块，因此模块单元及导入它们的文件可以像其他源文件一样工作：悬停、代码导航和代码补全都能跨越 `import`。各功能页面展示每项功能在模块单元中的表现；本页介绍模块本身。

<!-- The cards in the sections below are generated from the documented cases
     in tests/modules/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## 导入

导入模块的文件可以访问该模块导出的名称，以及该模块再导出的模块所导出的名称。

<!-- BEGIN GENERATED ITEMS: modules/imports -->

<!-- BEGIN CAPABILITY: supported -->

**导入的符号**

在导入模块的文件中，悬停在从模块导入的名称上即可查看其声明

导入模块的文件依据模块接口进行编译，clice 会在文件打开时构建该接口；无需打开其他文件。

```case
tests/modules/imports/01_imported_symbol
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**再导出的模块**

模块通过 `export import` 再导出另一个模块后，导入该模块的文件就可以访问这两个模块导出的名称

```case
tests/modules/imports/04_reexport
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 导出

声明可以单独导出，也可以放在 `export` 块中导出，或作为 `export namespace` 的成员导出。

<!-- BEGIN GENERATED ITEMS: modules/exports -->

<!-- BEGIN CAPABILITY: supported -->

**导出块和命名空间**

`export { … }` 块会导出其中的每个声明，`export namespace` 则会导出命名空间中的每个成员

```case
tests/modules/exports/01_export_forms
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块单元

模块可以拆分为多个分区，模块单元可以在其全局模块片段（global module fragment）中包含头文件。

<!-- BEGIN GENERATED ITEMS: modules/units -->

<!-- BEGIN CAPABILITY: supported -->

**模块分区**

对于拆分为多个分区的模块，导入该模块的文件可以访问其主接口再导出的所有分区中的声明

一个分区仅凭分区名即可导入同一模块中的另一个分区，如 `import :Types;`，主接口则使用各分区中的声明。

```case
tests/modules/units/01_partitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**全局模块片段**

模块单元在模块声明之前的全局模块片段中包含头文件，并在模块声明之后导入模块

```case
tests/modules/units/03_global_fragment
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**只编译一次的模块单元**

模块单元只编译一次，编译成导入它的文件所读取的模块文件，这次编译也同时为它建立索引

构建模块文件时会把该单元的索引合并进项目索引，后台索引随后会发现该单元的索引已是最新；导入该模块的文件则从其编译所加载的模块文件中补全模块导出的名字。

```case
tests/modules/units/08_indexed_by_its_build
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**由 cl 构建的模块单元**

对于借助 CMake 或 MSBuild 用 cl 构建的项目，clice 会像对待其他项目一样构建其模块，`.ixx` 接口也不例外

单元属于哪一种由其模块声明决定，与扩展名无关；cl 写出和读取的接口文件则交给构建处理。

```case
tests/modules/units/14_cl_module_commands
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 编辑

对模块接口的修改会在保存后反映到导入该模块的文件中。

<!-- BEGIN GENERATED ITEMS: modules/edits -->

<!-- BEGIN CAPABILITY: supported -->

**接口修改会反映到导入模块的文件中**

模块接口保存后，导入该模块的文件才能看到修改后的声明，修改尚未保存时则看不到

```case
tests/modules/edits/03_unsaved_interface
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 构建失败

模块构建失败时会报告错误，导入它的文件则会报告导入错误。

<!-- BEGIN GENERATED ITEMS: modules/failures -->

<!-- BEGIN CAPABILITY: supported -->

**出错的模块**

模块构建失败时会报告自身的错误，导入它的文件则会说明它为何没有构建成功

```case
tests/modules/failures/01_broken_module
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 已知限制

- **头文件单元**。不支持 `import "util.h";` 和 `import <util.h>;`。每条这样的导入都会报错，说明不支持头文件单元，导入之后也看不到该头文件声明的任何内容；请改为包含该头文件。
