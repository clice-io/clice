# C++20 模块

clice 会在文件打开时构建该文件导入的模块，因此模块单元及导入它们的文件可以像其他源文件一样工作：悬停、代码导航和代码补全都能跨越 `import`。各功能页面展示每项功能在模块单元中的表现；本页介绍模块本身。

<!-- The cards in the sections below are generated from the documented cases
     in tests-next/modules/. Do not edit the regions between the
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
tests-next/modules/imports/01_imported_symbol
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**再导出的模块**

模块通过 `export import` 再导出另一个模块后，导入该模块的文件就可以访问这两个模块导出的名称

```case
tests-next/modules/imports/04_reexport
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
tests-next/modules/exports/01_export_forms
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
tests-next/modules/units/01_partitions
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**全局模块片段**

模块单元在模块声明之前的全局模块片段中包含头文件，并在模块声明之后导入模块

```case
tests-next/modules/units/03_global_fragment
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: unsupported -->

**头文件单元**

目前尚不支持通过 `import "local.h";` 或 `import <util.h>;` 将头文件作为头文件单元导入

```case
tests-next/modules/units/09_header_units
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
tests-next/modules/edits/03_unsaved_interface
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 构建失败

模块构建失败时会报告错误，导入它的文件则会报告导入错误。

<!-- BEGIN GENERATED ITEMS: modules/failures -->

<!-- BEGIN CAPABILITY: partial -->

**出错的模块**

模块构建失败时会报告自身的错误，导入它的文件则会报告导入错误

```case
tests-next/modules/failures/01_broken_module
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
