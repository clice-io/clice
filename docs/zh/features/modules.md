# C++20 模块

clice 会在编译文件时构建该文件导入的模块，因此模块单元及其导入方可以像其他源文件一样工作：悬停、代码导航和代码补全都能跨越 `import`。各功能页面展示每项功能在模块单元中的表现；本页介绍模块本身。

<!-- The cards in the sections below are generated from the documented cases
     in tests/modules/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run docs-update`. -->

## 导入

导入模块的文件可以看到该模块导出的名称，以及它再导出的模块所导出的名称。

<!-- BEGIN GENERATED ITEMS: modules/imports -->

<!-- BEGIN CAPABILITY: supported -->

**导入的符号**

在导入模块的文件中，将光标悬停在从模块导入的名称上即可查看其声明

导入方文件针对模块接口编译，clice 会在打开该文件时构建该接口；无需打开其他文件。

```case
tests/modules/imports/01_imported_symbol
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**再导出的模块**

模块通过 `export import` 再导出另一个模块后，其导入方同时可见两个模块导出的名称

```case
tests/modules/imports/04_reexport
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 导出

声明可以单独导出，也可以放在 `export` 块中导出，或作为 `export namespace` 的成员导出。

<!-- BEGIN GENERATED ITEMS: modules/exports -->

<!-- BEGIN CAPABILITY: supported -->

**导出块与导出命名空间**

`export { … }` 块会导出其中的每条声明，`export namespace` 则会导出命名空间中的每个成员

```case
tests/modules/exports/01_export_forms
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 模块单元

模块可以拆分为多个分区（partition），模块单元也可以在全局模块片段（global module fragment）中包含头文件。

<!-- BEGIN GENERATED ITEMS: modules/units -->

<!-- BEGIN CAPABILITY: supported -->

**模块分区**

拆分为多个分区的模块会向导入方提供主接口再导出的各个分区中的声明

分区只需凭分区名即可导入同模块的其他分区（如 `import :Types;`），主接口则可使用各分区中的声明。

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

模块单元只编译一次，生成供导入方读取的模块文件，且同一次编译也会为该单元建立索引

构建模块文件时会将该单元的索引合并进项目索引，后台索引随后会视其为最新状态；导入该模块的文件在补全模块导出的名称时，直接读取其编译所加载的模块文件。

```case
tests/modules/units/08_indexed_by_its_build
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**由 cl 构建的模块单元**

使用 CMake 或 MSBuild 调用 cl 构建的项目，其模块（包括 `.ixx` 接口文件）会与其他项目一样构建

模块单元的种类取决于其模块声明，与文件后缀无关；cl 读写接口文件的行为则留给项目自身的构建处理。

```case
tests/modules/units/14_cl_module_commands
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 编辑

模块接口的修改在保存后即会反映到导入方文件中。

<!-- BEGIN GENERATED ITEMS: modules/edits -->

<!-- BEGIN CAPABILITY: supported -->

**接口修改反映到导入方**

模块接口保存后，导入方文件即可看到接口声明的更新；修改尚未保存时则不会生效

```case
tests/modules/edits/03_unsaved_interface
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 构建失败

构建失败的模块会报告其自身的错误，导入它的文件则会在导入处报错。

<!-- BEGIN GENERATED ITEMS: modules/failures -->

<!-- BEGIN CAPABILITY: supported -->

**出错的模块**

构建失败的模块会报告自身的错误，导入它的文件则会说明该模块未能构建的原因

```case
tests/modules/failures/01_broken_module
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 已知限制

- **头文件单元（header units）**。不支持 `import "util.h";` 和 `import <util.h>;`。每条此类导入都会报错指明不支持，且头文件中的声明在其后均不可见；请改为直接包含该头文件。
