# 从 clangd 迁移

为 clangd 配置的项目往往带有各种变通做法，用来弥补 clangd 自身无法直接读取的信息。clice 无需额外配置即可识别其中大多数情况；本页说明哪些变通做法可以移除，以及 `.clangd` 文件中的设置在 `clice.toml` 中如何书写。clice 不读取 `.clangd`。

## 可以移除的变通做法

### 编译数据库中的头文件条目

对于没有自身条目的头文件，clangd 会从附近的源文件猜测一条编译命令，因此不少项目会手动或借助工具把头文件加进 `compile_commands.json`。clice 则在包含该头文件的源文件上下文中编译头文件，使用该源文件的编译命令以及包含位置之前的声明（参见[头文件上下文](../features/header-contexts.md)）；尚无任何文件包含的头文件则借用附近翻译单元的编译命令。如果头文件在数据库中有自己的条目，该条目会优先于宿主上下文，还会使头文件成为独立的翻译单元，后台索引除了编译包含它的源文件之外还会单独编译它。生成编译数据库时不必写入头文件条目；如果构建系统仍然会写出它们，可以用规则将它们排除在后台索引之外：

```toml
[[rules]]
patterns = ["**/*.h", "**/*.hpp"]
background_index = false
```

### `--query-driver`

clangd 只有在 `--query-driver` 允许时才会向编译器查询系统头文件和目标平台，交叉编译器或安装在默认路径之外的 GCC 都需要配置它。clice 会向每条命令的编译器直接查询其目标平台、标准库与系统头文件、预定义宏以及默认语言标准。规则中的 `query_toolchain = false` 可针对匹配的文件关闭这一查询。命令中若在编译器前带有启动器（ccache、sccache、distcc 或 icecc），clice 会先剥离启动器，再向其后的编译器查询。

### 为 clang 移除的编译选项

`.clangd` 中常常需要从 GCC、cl 或 nvcc 的命令里移除 clang 无法识别的选项。clice 直接按原编译器的语义读取这些命令：

- GCC 支持而 clang 不认识的警告选项（`-Wlogical-op`、`-Wuseless-cast`）会被略去；GCC、cl 或 nvcc 升级为错误的警告（`-Werror`、`-Werror=shadow`、`/WX`）在编译时仍保留为警告，因为 clang 会报出那些编译器不报的警告。
- 只影响代码生成或在目标文件旁额外写出文件的选项——LTO 并行数、GCC 的模块映射器（module mapper）及依赖扫描输出、cl 的模块接口输出——均会被略去。
- GCC 的 `-fmodules` 按 C++20 模块解读，交叉编译 GCC 的 `-march` 和 `-mabi` 转换为 clang 接受的形式，nvcc 的选项（包括传给宿主编译器的选项）也按 nvcc 的方式读取。
- clang 完全不认识的选项会在编译时略去。

### `compile_commands.json` 符号链接

clangd 查找 `compile_commands.json` 时只看文件上方的各级目录及其 `build/` 子目录，因此位于别处的构建目录常常需要软链接到项目根目录。clice 会加载项目根目录及其所有直接子目录中的每一个 `compile_commands.json`（不论子目录名叫 `build/` 还是 `cmake-build-debug/`），也会加载已打开文件与根目录之间各级目录中的数据库；指向已发现数据库的符号链接视为同一份数据库。带有自身 `clice.toml` 或 `.clice/config.toml` 的子目录属于另一个项目；一旦有规则设置了 `compile_commands` 或 `default_command`，自动发现便会关闭。位置更深或位于项目之外的数据库可通过规则指定：

```toml
[[rules]]
compile_commands = ["out/release"]
```

同时发现两个构建目录时，每个文件默认按第一个构建目录编译，另一个则作为可切换的上下文提供；带标签的规则可将它们定义为不同的[构建配置](./configuration.md#switching-configurations)。

### 关闭合并构建（Unity Build）

合并构建在编译数据库中只列出包含各源文件的生成文件，因此 clangd 找不到源文件自身的编译命令，只能猜测一条。clice 处理编译数据库未列出的源文件时，与处理头文件一样，在包含它的合并文件上下文中编译它。

## `.clangd` 设置在 `clice.toml` 中的对应项

| `.clangd` 或 clangd 选项                 | `clice.toml`                                                  |
| ---------------------------------------- | ------------------------------------------------------------- |
| `If: PathMatch`                          | 规则的 `patterns`（使用 glob 模式，而非正则表达式）           |
| `CompileFlags: Add`                      | 规则的 `append`                                               |
| `CompileFlags: Remove`                   | 规则的 `remove`                                               |
| `CompileFlags: CompilationDatabase`      | 规则的 `compile_commands`                                     |
| `Index: Background: Skip`                | 规则的 `background_index = false`                             |
| `--background-index=false`               | 最后一条规则设为 `background_index = false` 且省略 `patterns` |
| `Diagnostics: Suppress`                  | `[diagnostics] suppress`                                      |
| `Diagnostics: ClangTidy`、`--clang-tidy` | 项目的 `.clang-tidy`；`[diagnostics] clang_tidy`              |
| `InlayHints`                             | `[inlay_hints]`                                               |
| `Hover: ShowAKA`                         | `[hover] show_aka`                                            |
| `Completion: AllScopes`                  | `[code_completion] all_scopes`                                |
| `--function-arg-placeholders=0`          | `[code_completion] arguments = "delimiters"`                  |
| `--limit-results`                        | `[code_completion] limit`                                     |
| `--limit-references`                     | `[references] limit`                                          |
| `-j`                                     | `[workers] background` 和 `[workers] max`                     |
