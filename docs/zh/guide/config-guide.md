# 配置指南

只要找到了编译数据库，clice 无需任何配置即可工作（参见[项目配置](./quick-start.md#project-setup)）。`clice.toml` 用来补充编译数据库没有说明的内容：它未列出的文件、需要修改的编译选项、要来回切换的多套构建，以及需要区别对待的代码。本页逐一介绍这些常见场景；所有选项见[配置参考](./configuration.md)。

## 配置文件

clice 在服务器启动时读取一次项目根目录下的 `clice.toml`，若该文件不存在则读取 `.clice/config.toml`；保存配置文件后，服务器会请求编辑器重启自身。编辑器也可以通过 `initializationOptions` 发送设置，这些设置会取代配置文件中的同名设置。

<!-- BEGIN GENERATED ITEMS: config/loading -->

<!-- BEGIN CAPABILITY: supported -->

**修改配置会重启服务器**

clice 在服务器启动时读取 `clice.toml`；保存该文件后，服务器会请求编辑器重启自身，修改将在新服务器中生效

服务器运行期间规则新增了该宏；编辑器重启服务器后，`main.cpp` 便带上该宏编译。

```case
tests/config/loading/04_edited_file
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

<!-- BEGIN GENERATED ITEMS: config/layers -->

<!-- BEGIN CAPABILITY: supported -->

**编辑器设置优先于文件配置**

编辑器发送的规则会替换 `clice.toml` 中的规则

`clice.toml` 为 `main.cpp` 提供了它用到的宏，文件得以正常编译。编辑器若在设置中发送自己的 `rules`，会直接替换配置文件中的规则而非追加，因此本例中的宏不再存在。

```case
tests/config/layers/01_editor_overrides
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

下文的大部分配置都写成规则。每个 `[[rules]]` 条目用 `patterns`（相对于项目根目录）选出文件，并为它们设置字段。匹配某个文件的所有规则按顺序生效：各规则的 `append` 和 `remove` 依次累加，其余字段由第一条设置了它的规则决定。因此针对部分文件的规则应写在针对全部文件的规则之前，不带 `patterns` 的规则放在最后。`clice command <file>` 会显示规则对某个文件的作用：编译数据库为它列出的每条命令（已应用规则的编辑），以及每项按文件设置的取值和决定它的规则。在 VS Code 中，Show Compile Command 会显示已打开文件实际编译所用的命令，无论该命令来自何处。

## 编译命令

clice 会加载项目根目录及其所有直接子目录中的每一个 `compile_commands.json`（不论子目录名叫 `build/` 还是 `cmake-build-debug/`），也会加载已打开文件与根目录之间各级目录中的数据库。位于别处的数据库通过规则指定，而一旦指定，自动发现便会关闭：

```toml
[[rules]]
compile_commands = ["out/release"]
```

多个数据库按书写顺序排定优先级：被多个数据库列出的文件默认按第一个数据库的命令编译，并可切换到其余数据库的命令。

编译数据库未列出的源文件使用规则的 `default_command` 编译，写法与命令行相同，只是不带源文件；它匹配到的磁盘上的源文件会加入后台索引。头文件则使用包含它的源文件的命令，没有源文件包含它时才使用 `default_command`：

```toml
[[rules]]
patterns = ["src/**", "include/**"]
default_command = "arm-none-eabi-gcc -std=c23 -mcpu=cortex-m4 -Iinclude"
```

`append` 为规则匹配的文件追加编译选项，`remove` 则从命令中移除选项：可以移除带特定值的选项（`-DDEBUG`）、某个选项的所有取值（`-I*` 或 `["-include", "*"]`），或以某个前缀开头的所有同类选项（`-W*` 会同时移除 `-Wall` 和 `-Wextra`）。同一条规则先移除再追加，后面规则的 `remove` 会抵消前面规则的 `append`：

```toml
[[rules]]
patterns = ["tests/**"]
append = ["-DTEST_MODE"]

[[rules]]
patterns = ["legacy/**"]
remove = ["-W*"]
```

<!-- BEGIN GENERATED ITEMS: project/rules -->

<!-- BEGIN CAPABILITY: supported -->

**没有编译数据库的文件**

规则匹配到的文件使用该规则的默认命令编译，后续新建的文件也会加入索引

`src/main.cpp` 按规则指定的命令编译，不会提示命令是推测得出的。服务器运行期间新建的文件打开时立即按同样方式编译，并在下次后台扫描磁盘时加入索引。

```case
tests/project/rules/01_default_command
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**按优先级排列的编译数据库**

出现在多个编译数据库中的文件默认取第一个数据库的命令，其余数据库的命令仍作为可切换上下文提供

编译数据库 `a` 为 `main.cpp` 列出的命令带有 `-DFROM_A`；编译数据库 `b` 为其列出的命令带有 `-DFROM_B`，并额外列出了 `other.cpp`。`main.cpp` 默认使用 `a` 中的命令编译，也可切换到 `b` 中的命令，而 `other.cpp` 使用 `b` 中的命令。

```case
tests/project/rules/03_database_order
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**匹配模式以项目根目录为起点**

相对模式匹配项目根目录（即 `clice.toml` 所在目录）下的路径

规则为 `src/` 下的文件追加 `-DFEATURE`：`src/main.cpp` 正常编译，而与 `clice.toml` 同级的 `other.cpp` 没有获得该宏。

```case
tests/project/rules/05_relative_patterns
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**从编译命令中移除选项**

规则可以从文件的编译命令中移除选项，且后续规则的移除操作可抵消前面规则追加的选项

编译数据库给 `main.cpp` 的命令同时包含 `-DBROKEN` 和 `-include prefix.h`，第一条规则将它们移除后文件得以编译通过。针对 `other.cpp`，第二条规则追加了 `-DFEATURE`，第三条规则又将其移除。

```case
tests/project/rules/09_remove_flags
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 多套构建

同时发现两个构建目录时，每个文件默认按第一个构建目录编译，另一个则作为可切换的上下文提供。带标签的规则可以把它们定义为构建配置，每套配置各有一份独立的索引：

```toml
default_configuration = "debug"

[[rules]]
configuration = "debug"
compile_commands = ["build/debug"]

[[rules]]
configuration = "release"
compile_commands = ["build/release"]
```

规则上的 `configuration` 标签构成一份配置菜单，每个项目同一时刻只有一个标签生效；不带标签的规则始终生效。活动配置按优先级依次取 `--configuration <tag>` 参数（`clice serve` 及所有操作项目的子命令均支持该参数）、持久化保存的选择，或 `default_configuration`。切换操作在编辑器内完成——在 VS Code 中使用编辑器右键菜单或命令面板里的 **Switch Build Configuration**（它会列出所有配置并标出当前生效项），其他客户端则调用 `clice/switchConfiguration`——所选结果保存在 `cache_dir` 下的 `state.json` 中，绝不会改动 `clice.toml`；服务器随后会像配置文件发生修改时一样请求编辑器重启自身，新配置在重启后生效。每套配置在 `cache_dir` 下各自拥有独立的索引，来回切换不会重复索引先前已索引过的内容。批处理命令同样借助这些标签选择索引：`clice index --configuration release` 构建 release 配置的索引，`clice index stats` 则报告按同一规则解析出的配置索引信息。

也有一些构建系统靠把符号链接指向不同构建目录来切换构建。设有 `resolve_symlinks = true` 的规则会按其 `compile_commands` 经符号链接和 Windows junction 解析出的真实路径区分构建：链接指向过的每个构建目录各自保留一份索引；链接改指他处时，运行中的服务器会请求重启以切换到新目标构建的索引；链接重新指回原先的构建时，先前的索引原封不动立即可用。若另一套配置的规则仅在直接写明该构建的编译数据库路径这一点上有所不同，则会与它共享同一份索引。在 VS Code 中，Switch Build Configuration 会显示当前活动配置的链接解析到了哪个构建，并标出与之共享索引的配置。所有构建索引都会一直保留直至手动删除：每份索引对应 `cache_dir` 下 `cache/v<N>/index` 中的一个子目录，以配置名和构建路径末尾几级目录命名。

<!-- BEGIN GENERATED ITEMS: project/configurations -->

<!-- BEGIN CAPABILITY: supported -->

**每块板卡一条命令**

带标签的规则构成构建配置菜单，切换配置会在服务器下次启动时生效

每块板卡的规则给出了各自的默认命令，`board-a` 为默认配置。在该配置下，头文件中的 `BOARD_B` 分支处于非活动状态；切换到 `board-b` 并重启后，变为 `BOARD_A` 分支处于非活动状态。

```case
tests/project/configurations/03_board_defaults
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**每个链接到的构建各有一份索引**

开启符号链接解析的规则会为链接指向过的每个构建各保留一份索引，并在链接改指时随之切换

构建系统将 `out/current` 指向当前选定的构建目录。当它指向 `out/debug`（`debug` 配置也直接指定了该目录）时，服务器切换到该配置的索引；指回 `out/release` 时，release 索引重新提供服务，无需重新索引任何内容。

```case
tests/project/configurations/07_linked_build
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 第三方代码

只会被调用的代码（例如放在 `third_party/` 下的第三方库）既不需要在每次编译中都解析函数体，也不需要 Lint 和格式化：

```toml
[[rules]]
patterns = ["third_party/**", "external/**"]
skip_function_body = true
lint = false
format = false
```

`skip_function_body = true` 会让除文件自身编译之外的每次编译都跳过匹配文件中的函数体（包括模板），编辑器和后台索引都是如此：项目中的翻译单元只解析所包含库的声明，导入的第三方库模块在构建时也会跳过函数体。转发实参的函数模板（如 `std::make_unique`）仍会解析，`constexpr` 函数、`consteval` 函数以及返回 `auto` 的函数也一样。

跳过函数体的代价：这些函数体内部的错误不会报告，你的代码实例化其模板时触发的错误也不会报告，函数体内部使用的名称同样不会建立索引——查找引用和调用层次结构会止步于它们的声明。`skip_function_body = false` 会在所有地方关闭跳过，包括编辑器自身的编译：默认情况下，编辑器会跳过已打开文件顶部所包含头文件中的函数体，改由后台索引解析它们，并把在其中发现的问题报告在按自身数据库条目编译的文件上。

`lint = false` 让这些文件不参与 `clice lint` 和编辑器中的 clang-tidy 检查，`format = false` 让它们不参与 `clice format`。

## 加快首次索引

超出已打开文件范围的响应——查找引用、调用层次结构与类型层次结构、工作区符号、重命名，以及在尚未编译的文件上的悬停或代码导航——都来自 clice 为项目建立的索引。服务器启动时，后台索引会编译项目中已保存索引尚未记录为最新的每个翻译单元：编译数据库中的每个文件、规则的 `default_command` 覆盖的源文件，以及此前打开过但未列在任何编译数据库中的文件。任务入队并经过 `[workers] background_delay_ms` 后后台索引开始运行，使用后台 worker，并为你正在编辑的文件上的请求让路。头文件通过包含它的翻译单元建立索引，每种包含方式各索引一次，因此被不同源文件以不同方式编译的头文件，能针对其中每个上下文给出结果。

索引保存在缓存目录中，构建期间每五分钟保存一次，运行结束时也会保存；下次启动的服务器或 `clice index` 运行只重新编译此后文件发生变化的翻译单元。因此大型项目的首次索引是一次性开销：它要把每个翻译单元编译一遍，中途中断的运行会从中断处继续。[`clice index`](../cli/index.md) 可以在命令行中提前完成这项工作；若同一版本 clice 的服务器正在服务该项目，它会请求该服务器代为完成。会话日志记录了耗时分布：每个翻译单元一行，写明其耗时；环境变量中设置 `CLICE_PERF=1` 时，每个翻译单元还会额外输出一行 `[perf:index]`，将耗时拆分为建立索引和合并两段，整次运行结束时也汇总一行。有五件事可以缩短首次索引时间：

- **更多 worker。** `[workers] background` 控制服务器同时编译的翻译单元数量：默认为机器物理核心数的一半，至少为 2，且不超过 `[workers] max`（默认为全部核心数）。如果机器核心多且内存充裕，可以调高它。`clice index` 同时编译 `--jobs` 个翻译单元，默认等于机器的逻辑 CPU 数。
- **充足的内存。** 只有机器剩余内存足够时才会启动新的翻译单元编译，内存吃紧时最新启动的任务会被中止，因此在内存不足的机器上增加 worker 并无帮助；后台任务因等待内存而暂停时会写入日志。如果某个翻译单元连单独运行都放不下，`clice index` 会指出它的名字以及其 worker 达到的内存占用。
- **更少的翻译单元。** 设有 `background_index = false` 的规则会让匹配的文件不作为独立翻译单元编译，适合你不会在其中导航的测试、示例、基准测试或生成源文件。它们包含的头文件仍会通过包含该头文件的其他翻译单元建立索引；模块单元或生成预编译头文件的单元，在其他翻译单元需要其模块或预编译头文件时仍会编译。有些构建生成器会把头文件也写进编译数据库，使每个头文件在包含它的源文件之外又成为独立的翻译单元；针对 `**/*.h` 设置 `background_index = false` 的规则即可排除它们。
- **跳过函数体。** 为[第三方代码](#third-party-code)设置 `skip_function_body = true` 后，你的翻译单元只需解析它的声明。
- **在 Windows 上免去实时扫描。** 文件写入或修改后，程序第一次读取它时 Microsoft Defender 会扫描它，而 clice 启动时会读取项目中的每个源文件和头文件：大型项目的文件若尚未被 Defender 扫描过，启动会慢上许多倍。把项目目录加入实时保护的排除项（Windows 安全中心 → 病毒和威胁防护设置 → 排除项）即可避免，代价是 Defender 不再扫描该目录中的文件。

```toml
[workers]
background = 16

[[rules]]
patterns = ["third_party/**", "external/**"]
skip_function_body = true
background_index = false

[[rules]]
patterns = ["benchmarks/**", "**/*.h"]
background_index = false
```

同一时间只有一个进程写入项目的索引。针对同一项目启动的第二个服务器（例如第二个编辑器窗口）最多等待五秒，让第一个进程释放索引，这足以让重启前的旧服务器保存完毕；若超时后索引仍被占用，第二个服务器不会服务该项目的任何内容，并指明占用索引的进程。

## 库的内部实现

索引为每个文件记录其声明、定义和引用、调用关系、继承关系，以及各翻译单元产生的模板实例化。在函数体内部，索引会记录函数体用到的名称，但系统头文件（标准库、`-isystem` 目录）除外：其中的函数体，以及写在其中的模板所产生的实例化，都不建立索引，而声明、类型和模板本身仍会建立索引。要在这类库内部导航——查找库内某个函数的调用方，或其模板的各处实例化——可通过规则为它的函数体建立索引：

```toml
[[rules]]
patterns = ["/usr/include/c++/**"]
index_function_body = true
```

每多记录一个函数体，索引体积都会相应增大，因此最好只对你会阅读的库开启。

## 从 clangd 迁移

为 clangd 配置的项目往往带有各种变通做法，用来弥补 clangd 自身无法直接读取的信息。clice 无需额外配置即可识别其中大多数情况；本节说明哪些变通做法可以移除，以及 `.clangd` 文件中的设置在 `clice.toml` 中如何书写。clice 不读取 `.clangd`。

### 编译数据库中的头文件条目

对于没有自身条目的头文件，clangd 会从附近的源文件猜测一条编译命令，因此不少项目会手动或借助工具把头文件加进 `compile_commands.json`。clice 则在包含该头文件的源文件上下文中编译头文件，使用该源文件的编译命令以及包含位置之前的声明（参见[头文件上下文](../features/header-contexts.md)）；尚无任何文件包含的头文件则借用附近翻译单元的编译命令。如果头文件在数据库中有自己的条目，该条目会优先于宿主上下文，还会使头文件成为独立的翻译单元，后台索引除了编译包含它的源文件之外还会单独编译它。生成编译数据库时不必写入头文件条目；如果构建系统仍然会写出它们，可以用规则将它们排除在后台索引之外（参见[更少的翻译单元](#a-faster-first-index)）。

### `--query-driver`

clangd 只有在 `--query-driver` 允许时才会向编译器查询系统头文件和目标平台，交叉编译器或安装在默认路径之外的 GCC 都需要配置它。clice 会向每条命令的编译器直接查询其目标平台、标准库与系统头文件、预定义宏以及默认语言标准。规则中的 `query_toolchain = false` 可针对匹配的文件关闭这一查询。命令中若在编译器前带有启动器（ccache、sccache、distcc 或 icecc），clice 会先剥离启动器，再向其后的编译器查询。

### 为 clang 移除的编译选项

`.clangd` 中常常需要从 GCC、cl 或 nvcc 的命令里移除 clang 无法识别的选项。clice 直接按原编译器的语义读取这些命令：

- GCC 支持而 clang 不认识的警告选项（`-Wlogical-op`、`-Wuseless-cast`）会被略去；GCC、cl 或 nvcc 升级为错误的警告（`-Werror`、`-Werror=shadow`、`/WX`）在编译时仍保留为警告，因为 clang 会报出那些编译器不报的警告。
- 只影响代码生成或在目标文件旁额外写出文件的选项——LTO 并行数、GCC 的模块映射器（module mapper）及依赖扫描输出、cl 的模块接口输出——均会被略去。
- GCC 的 `-fmodules` 按 C++20 模块解读，交叉编译 GCC 的 `-march` 和 `-mabi` 转换为 clang 接受的形式，nvcc 的选项（包括传给宿主编译器的选项）也按 nvcc 的方式读取。
- clang 完全不认识的选项会在编译时略去。

### `compile_commands.json` 符号链接

clangd 查找 `compile_commands.json` 时只看文件上方的各级目录及其 `build/` 子目录，因此位于别处的构建目录常常需要用符号链接链到项目根目录。clice 会在项目根目录的每个直接子目录中查找数据库，不论子目录叫什么名字，指向已发现数据库的符号链接视为同一份数据库；位置更深或位于项目之外的数据库可通过规则指定（参见[编译命令](#compile-commands)）。

### 关闭合并构建（Unity Build）

合并构建在编译数据库中只列出包含各源文件的生成文件，因此 clangd 找不到源文件自身的编译命令，只能猜测一条。clice 处理编译数据库未列出的源文件时，与处理头文件一样，在包含它的合并文件上下文中编译它。

### `.clangd` 设置在 `clice.toml` 中的对应项

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
