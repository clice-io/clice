# 贡献

我们欢迎任何贡献！

请参考 [build](./build.md) 构建 clice，参考 [test and debug](./test-and-debug.md) 运行测试。

## 提交前

1. **格式化**：运行 `pixi run format` 格式化所有源文件。
2. **测试**：四种测试套件——单元、集成、冒烟和快照——必须全部通过（`pixi run test`）。
3. **提交信息**：使用 [conventional commits](https://www.conventionalcommits.org/)：

   ```
   <type>(<scope>): <short description>
   ```

   类型：`feat`、`fix`、`refactor`、`chore`、`docs`、`ci`、`test`、`perf`

   作用域：匹配 `src/` 子目录或功能名（如 `completion`、`server`、`index`）。

   标题行不超过 70 个字符。

## 代码风格

### 命名

| 实体             | 规范         | 示例                              |
| ---------------- | ------------ | --------------------------------- |
| 变量、字段、函数 | `snake_case` | `file_path`                       |
| 类、枚举、概念   | `PascalCase` | `CompileGraph`、`SymbolKind`      |
| 枚举值           | `PascalCase` | `GoToDefinition`、`IncludeAngled` |
| 模板参数         | `PascalCase` | `typename Result`                 |

类成员字段不使用前缀或后缀（不用 `m_`，不用尾随 `_`）。

### 字符串参数

优先级：`llvm::StringRef` > `std::string_view` > `const std::string&`。

### 现代 C++

- 目标 C++23。使用 `std::ranges`、`std::expected`、`std::format`。
- 适当情况下优先使用 LLVM 数据结构（`SmallVector`、`DenseMap`、`StringRef`）。
- 不使用 `<iostream>` 或 C 风格 I/O。
- 优先使用原始字符串字面量 `R"(...)"` 而非转义字符串。

### 错误处理

- 使用带初始化语句的 `if` 来限定错误变量的作用域。
- 保持 early-return / 扁平控制流——避免深层嵌套。

## 插件开发

参见 [extension](./extension.md) 了解编辑器插件开发。
