# 重命名

重命名符号（`textDocument/rename`，在 VS Code 中按 `F2`）会根据项目索引，修改整个工作区中所有使用该符号名称的位置。[`clice refactor rename`](../cli/refactor.md) 在命令行中使用同一套引擎；其文档页面详细说明了重命名的关联范围，以及哪些情况会被拒绝。

<!-- The cards in the sections below are generated from the documented cases
     in tests-next/rename/. Do not edit the regions between the
     GENERATED markers by hand — edit the cases' README.md and run
     `pixi run update-feature-docs`. -->

## 修改内容

重命名会修改符号的声明、定义和所有使用位置，并重命名必须与它保持同名的符号。

<!-- BEGIN GENERATED ITEMS: rename/edits -->

<!-- BEGIN CAPABILITY: supported -->

**跨文件重命名**

重命名函数会修改工作区中该函数的声明、定义和所有调用；宏内出现的名称保持不变，并会在消息中指出

```case
tests-next/rename/edits/01_across_files
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**重命名的关联范围**

重命名类会同时重命名其构造函数和析构函数；重命名模板会同时重命名其特化；重命名虚函数会同时重命名覆盖它的函数

```case
tests-next/rename/edits/03_followers
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 拒绝重命名的情况

如果符号无法重命名，或新名称不合适，重命名会被拒绝并说明原因。

<!-- BEGIN GENERATED ITEMS: rename/refusals -->

<!-- BEGIN CAPABILITY: supported -->

**不允许重命名的名称**

在宏、运算符或系统头文件中的符号上触发重命名时，会提示无法重命名的原因；光标不在任何名称上时，不会执行任何操作

```case
tests-next/rename/refusals/01_unrenamable_symbols
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**不允许使用的新名称**

如果新名称不是合法标识符、属于关键字、在同一作用域内已被占用，或是宏名，重命名会被拒绝并说明原因；如果会形成函数重载或使用保留名称，则会执行重命名并发出警告

```case
tests-next/rename/refusals/02_rejected_names
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
