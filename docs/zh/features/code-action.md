# 代码操作

clice 尚未声明支持 `textDocument/codeAction`。目前，直接发送的请求会进入存根处理器并返回空列表。本页记录计划支持的范围。

## 快速修复

由 clang / clang-tidy 诊断附带的 `FixItHint` 派生而来的操作。

- [ ] 将编译器的 `FixItHint` 作为快速修复应用
- [ ] 应用 clang-tidy 修复建议
- [ ] `source.fixAll` — 批量应用文件中的所有可用修复（[clangd#1446](https://github.com/clangd/clangd/issues/1446)）
- [ ] 一次性修复同类诊断的所有实例（[clangd#830](https://github.com/clangd/clangd/issues/830)）
- [ ] 应用会修改主文件以外内容的修复（[clangd#1747](https://github.com/clangd/clangd/issues/1747)）
- [ ] 遵循客户端的代码操作能力（`isPreferred`、resolve 支持）（[clangd#573](https://github.com/clangd/clangd/issues/573)）
- [ ] 可选择格式化代码操作生成的编辑（[clangd#2476](https://github.com/clangd/clangd/issues/2476)）

## 包含指令操作

- [ ] 为未解析的符号添加缺失的 `#include`（[clangd#1017](https://github.com/clangd/clangd/issues/1017)）
- [ ] 插入包含指令时使用项目相对路径，而非绝对路径（[clangd#2010](https://github.com/clangd/clangd/issues/2010)）
- [ ] 包含指令样式可配置——双引号或尖括号（[clangd#1367](https://github.com/clangd/clangd/issues/1367)）
- [ ] 移除未使用的 `#include`（include-cleaner）
- [ ] 遵循 IWYU pragma（`export`、`keep`、`private`）
- [ ] 建议使用 `using` 声明，作为对未解析符号进行限定的替代方案（[clangd#976](https://github.com/clangd/clangd/issues/976)）
- [ ] C 文件：建议 `<stdlib.h>` 而非 `<cstdlib>`（[clangd#2246](https://github.com/clangd/clangd/issues/2246)）

## 重构

由光标或选区驱动的重构操作。

### 提取

- [ ] 提取变量（[clangd#446](https://github.com/clangd/clangd/issues/446)）
- [ ] 提取变量时应替换该表达式的所有出现位置（[clangd#924](https://github.com/clangd/clangd/issues/924)）
- [ ] 从宏实参中提取变量（[clangd#1197](https://github.com/clangd/clangd/issues/1197)）
- [ ] 提取函数或方法（[clangd#698](https://github.com/clangd/clangd/issues/698)）
- [ ] 提取函数应保留占位返回类型（`auto`）（[clangd#653](https://github.com/clangd/clangd/issues/653)）
- [ ] 提取函数不得引入脱糖后的类型（[clangd#1496](https://github.com/clangd/clangd/issues/1496)）
- [ ] 提取函数须处理在外层作用域定义的类型（[clangd#1710](https://github.com/clangd/clangd/issues/1710)）
- [ ] 在 C 文件中提取函数（[clangd#1810](https://github.com/clangd/clangd/issues/1810)）

### 内联 / 展开

- [ ] 内联变量或函数
- [ ] 展开 `auto` 或推导类型
- [ ] 将宏展开一层（[clangd#820](https://github.com/clangd/clangd/issues/820)）

### 移动 / 定义

- [ ] 在类外定义方法（将方法体移出类）
- [ ] 内联定义方法（将方法体移入声明中）
- [ ] 根据声明生成缺失的方法定义（[clangd#445](https://github.com/clangd/clangd/issues/445)）
- [ ] 根据类外定义生成缺失的声明（[clangd#2454](https://github.com/clangd/clangd/issues/2454)、[clangd#730](https://github.com/clangd/clangd/issues/730)）

### 转换

- [ ] 添加 `using` 声明（[clangd#73](https://github.com/clangd/clangd/issues/73)）
- [ ] 通过就地限定名称来替换 `using namespace`（[clangd#1067](https://github.com/clangd/clangd/issues/1067)）
- [ ] 移除不必要的类型限定符（[clangd#1619](https://github.com/clangd/clangd/issues/1619)）
- [ ] 交换 `if`/`else` 分支（[clangd#466](https://github.com/clangd/clangd/issues/466)）
- [ ] 补全 `switch` 分支（[clangd#807](https://github.com/clangd/clangd/issues/807)）
- [ ] 转换为原始字符串字面量
- [ ] 根据使用处创建声明（[clangd#467](https://github.com/clangd/clangd/issues/467)）
- [ ] 移除函数/方法（[clangd#2580](https://github.com/clangd/clangd/issues/2580)）
- [ ] 修改函数参数并更新调用点（[clangd#460](https://github.com/clangd/clangd/issues/460)）
- [ ] 修复声明与定义的签名不匹配问题（[clangd#77](https://github.com/clangd/clangd/issues/77)）
- [ ] 为基类的纯虚方法生成存根（[clangd#1037](https://github.com/clangd/clangd/issues/1037)）
- [ ] 交换二元运算符的操作数
- [ ] 将无作用域枚举转换为有作用域枚举
- [ ] 生成逐成员构造函数
- [ ] 声明隐式的拷贝/移动特殊成员函数
- [ ] 重命名符号（作为代码操作）
- [ ] Include-cleaner：批量修复未使用或缺失的 include
