# 参与贡献

欢迎任何形式的贡献！

请参考[构建](./build.md)文档构建 clice，并参考[测试与调试](./test-and-debug.md)文档运行测试。

## 提交前

1. **格式化**：运行 `pixi run format` 格式化所有源文件。
2. **测试**：单元测试、集成测试、冒烟测试和快照测试这四套测试必须全部通过（`pixi run test`）。
3. **提交信息**：使用[约定式提交](https://www.conventionalcommits.org/)：

   ```
   <type>(<scope>): <short description>
   ```

   类型：`feat`、`fix`、`refactor`、`chore`、`docs`、`ci`、`test`、`perf`

   作用域：与 `src/` 下的子目录或功能名一致（例如 `completion`、`server`、`index`）。

   主题行应少于 70 个字符。

## 代码风格

### 命名

| 实体             | 命名规范     | 示例                              |
| ---------------- | ------------ | --------------------------------- |
| 变量、字段、函数 | `snake_case` | `file_path`                       |
| 类、枚举、概念   | `PascalCase` | `CompileGraph`、`SymbolKind`      |
| 枚举值           | `PascalCase` | `GoToDefinition`、`IncludeAngled` |
| 模板参数         | `PascalCase` | `typename Result`                 |

类成员字段不使用前缀或后缀（不加 `m_`，末尾也不加 `_`）。

### 字符串参数

优先顺序为 `llvm::StringRef` > `std::string_view` > `const std::string&`。

### 现代 C++

- 以 C++23 为目标标准。使用 `std::ranges`、`std::expected`、`std::format`。
- 适当情况下优先使用 LLVM 数据结构（`SmallVector`、`DenseMap`、`StringRef`）。
- 不使用 `<iostream>` 或 C 风格 I/O。
- 优先使用原始字符串字面量 `R"(...)"`，而非转义字符串。

### 错误处理

- 使用带初始化语句的 `if`，将错误变量限定在适当的作用域内。
- 采用提前返回和扁平化控制流，避免深层嵌套。

## 插件开发

有关编辑器插件开发，请参阅[插件](./extension.md)文档。
