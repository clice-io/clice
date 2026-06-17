# 代码操作

clice 已注册 `textDocument/codeAction` 支持，但目前始终返回空列表。本页记录预期范围。

## 快速修复

从附加到 clang / clang-tidy 诊断的 `FixItHint` 派生的操作。

- [ ] 将编译器 `FixItHint` 作为快速修复应用
- [ ] 应用 clang-tidy 修复建议
- [ ] `source.fixAll` — 批量应用文件中所有可用修复（[clangd#1446](https://github.com/clangd/clangd/issues/1446)）
- [ ] 一次操作修复同类诊断的所有出现（[clangd#830](https://github.com/clangd/clangd/issues/830)）
- [ ] 应用编辑位于主文件之外的修复（[clangd#1747](https://github.com/clangd/clangd/issues/1747)）
- [ ] 遵守客户端代码操作能力（`isPreferred`、resolve 支持）（[clangd#573](https://github.com/clangd/clangd/issues/573)）
- [ ] 可选对代码操作编辑应用格式化（[clangd#2476](https://github.com/clangd/clangd/issues/2476)）

## Include 操作

- [ ] 为未解析的符号添加缺失的 `#include`（[clangd#1017](https://github.com/clangd/clangd/issues/1017)）
- [ ] 使用项目相对路径而非绝对路径插入 include（[clangd#2010](https://github.com/clangd/clangd/issues/2010)）
- [ ] 可配置的 include 样式 — 引号 vs. 尖括号（[clangd#1367](https://github.com/clangd/clangd/issues/1367)）
- [ ] 移除未使用的 `#include`（include-cleaner）
- [ ] 遵守 IWYU pragma（`export`、`keep`、`private`）
- [ ] 建议 `using` 声明作为限定未解析符号的替代（[clangd#976](https://github.com/clangd/clangd/issues/976)）
- [ ] C 文件：建议 `<stdlib.h>` 而非 `<cstdlib>`（[clangd#2246](https://github.com/clangd/clangd/issues/2246)）

## 重构

光标/选区驱动的重构操作。

### 提取

- [ ] 提取变量（[clangd#446](https://github.com/clangd/clangd/issues/446)）
- [ ] 提取变量应替换表达式的所有出现（[clangd#924](https://github.com/clangd/clangd/issues/924)）
- [ ] 宏参数中的变量提取（[clangd#1197](https://github.com/clangd/clangd/issues/1197)）
- [ ] 提取函数/方法（[clangd#698](https://github.com/clangd/clangd/issues/698)）
- [ ] 提取函数应保留占位返回类型（`auto`）（[clangd#653](https://github.com/clangd/clangd/issues/653)）
- [ ] 提取函数不得引入脱糖类型（[clangd#1496](https://github.com/clangd/clangd/issues/1496)）
- [ ] 提取函数须处理在外层作用域定义的类型（[clangd#1710](https://github.com/clangd/clangd/issues/1710)）
- [ ] C 文件的函数提取（[clangd#1810](https://github.com/clangd/clangd/issues/1810)）

### 内联 / 展开

- [ ] 内联变量/函数
- [ ] 展开 `auto` / 推导类型
- [ ] 展开宏一层（[clangd#820](https://github.com/clangd/clangd/issues/820)）

### 移动 / 定义

- [ ] 将方法体移到类外（out-of-line）
- [ ] 将方法体移到声明处（inline）
- [ ] 从声明生成缺失的方法定义（[clangd#445](https://github.com/clangd/clangd/issues/445)）
- [ ] 从类外定义生成缺失的声明（[clangd#2454](https://github.com/clangd/clangd/issues/2454)、[clangd#730](https://github.com/clangd/clangd/issues/730)）

### 变换

- [ ] 添加 `using` 声明（[clangd#73](https://github.com/clangd/clangd/issues/73)）
- [ ] 将 `using namespace` 替换为逐个 `using` 声明（[clangd#1067](https://github.com/clangd/clangd/issues/1067)）
- [ ] 去除限定符或替换为类型别名（[clangd#1619](https://github.com/clangd/clangd/issues/1619)）
- [ ] 交换/反转 `if` 分支（[clangd#466](https://github.com/clangd/clangd/issues/466)）
- [ ] 填充 `switch` 分支（[clangd#807](https://github.com/clangd/clangd/issues/807)）
- [ ] 普通字符串与原始字符串字面量互转
- [ ] 从使用处创建声明（[clangd#467](https://github.com/clangd/clangd/issues/467)）
- [ ] 移除函数/方法（[clangd#2580](https://github.com/clangd/clangd/issues/2580)）
- [ ] 修改函数参数并更新所有调用点（[clangd#460](https://github.com/clangd/clangd/issues/460)）
- [ ] 修复声明/定义签名不匹配（[clangd#77](https://github.com/clangd/clangd/issues/77)）
- [ ] 实现基类的纯虚方法（[clangd#1037](https://github.com/clangd/clangd/issues/1037)）

## 变更记录

| 日期 | 变更 | PR |
| ---- | ---- | -- |
| —    | 存根处理器（始终返回空列表） | — |
