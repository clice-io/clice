---
outline: 2
---

# 配置

clice 从项目根目录的 `clice.toml` 读取配置；若该文件不存在，则从 `.clice/config.toml` 读取。配置也可以通过 LSP `initializationOptions`（JSON 格式）传入；`initializationOptions` 中的值覆盖配置文件，合并后仍未设置的项由默认值填充。

配置只在服务器启动时读取一次，没有热重载。某个项目的配置文件被保存、创建或删除时，服务器会请编辑器重启它：VS Code（通过 socket 连接服务器时除外）和 Neovim 0.12 及以上版本会自行重启；其他编辑器会在该文件上显示一条诊断，说明修改要等 clice 重启后才生效。

clice 无法遵照的配置是错误：无法解析的文件、类型错误的值、超出选项可选范围的值、为零的 worker 数量、无法编译的模式、所指标签没有任何规则声明的 `default_configuration`。服务器把每个问题显示为该文件上的一条错误诊断，在修正后的文件保存之前不为该项目提供任何服务；命令行运行则逐一给出每个问题及其位置，并以退出码 2 退出。clice 不认识的键只在文件上给出一条警告，其余配置照常生效。`initializationOptions` 中有错误的设置会被整体舍弃，错误信息会指出它们。

完整配置的 JSON Schema 发布于 [`clice-config.schema.json`](/clice-config.schema.json)；支持根据 Schema 验证 TOML 或 JSON 的编辑器可以使用它。VS Code 扩展为 [Even Better TOML](https://marketplace.visualstudio.com/items?itemName=tamasfe.even-better-toml) 把它关联到这两个文件，后者随即为它们提供补全和校验；在其他编辑器中，在文件第一行写上 `#:schema https://docs.clice.io/clice-config.schema.json`，即可让 taplo（Zed 和 Neovim 常用的 TOML 语言服务器）使用它。

文件中的相对路径和模式相对于配置文件自身所在的目录解析；通过 `initializationOptions` 传入的值则相对于项目根目录解析。

## 编辑器设置

编辑器通过 `initializationOptions` 传入的设置会覆盖文件中的同名设置。

<!-- BEGIN GENERATED ITEMS: config/layers -->

<!-- BEGIN CAPABILITY: supported -->

**编辑器设置优先于文件配置**

编辑器发送的规则会替换 `clice.toml` 中的规则

`clice.toml` 为 `main.cpp` 提供了它使用的宏，因此该文件可以编译通过。如果编辑器在设置中发送自己的 `rules`，就会替换文件中的规则，而不是追加规则，因此本例中的宏会丢失。

```case
tests/config/layers/01_editor_overrides
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 重启

对配置的修改会在服务器下次启动时生效。

<!-- BEGIN GENERATED ITEMS: config/loading -->

<!-- BEGIN CAPABILITY: supported -->

**修改会让服务器重启**

clice 在服务器启动时读取 `clice.toml`。保存该文件后，服务器会请编辑器重启它，修改在新的服务器中生效

服务器运行期间，规则加上了这个宏；编辑器重启服务器之后，`main.cpp` 就用上它编译。

```case
tests/config/loading/04_edited_file
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 多个文件夹

编辑器打开的每个工作区文件夹，只要含有 `clice.toml` 或 `compile_commands.json`（位于该文件夹本身或其直接子目录中，即发现机制会查找的位置），就是一个独立的项目，各有自己的 `clice.toml`、编译数据库和缓存目录。两者都没有、且位于另一个已打开文件夹之内的文件夹，则属于那个文件夹的项目；而在一个文件夹内部，含有自己的 `clice.toml` 或数据库的目录，会作为独立的项目，服务那些该文件夹的项目不构建的文件。在所有文件夹之外打开的文件，归属于其上方最近的、含有 `clice.toml` 或 `compile_commands.json`（直接位于该目录中，或位于其 `build/` 目录中）的目录；随后该目录会像已打开的文件夹一样得到服务，除非某个正在服务的项目已经加载了那份数据库。文件由数据库中列有它的项目负责编译；没有自身条目的头文件，会从源文件包含了它的项目中借用宿主，并优先选择文件夹中含有该头文件的项目。工作区符号搜索覆盖所有项目；导航——查找引用、跳转到定义与声明、调用层级与类型层级、查找实现——会延伸到那些在同一文件中声明了该符号的项目。编辑器中打开的文件只通过服务它的项目作答，因此每个查询看到的都是它未保存的修改。

`initializationOptions` 作用于每个项目，而一个缓存目录只服务一个项目：位于项目之外的缓存目录会记下最先使用它的项目；其他项目——无论是在同一个 server 中，还是在之后的一次 `clice index` 运行中——都会先退回到自身 `clice.toml` 指定的缓存目录，再退回到其文件夹中的 `.clice`；这些也都被占用时，就在没有缓存的情况下运行。因此，绝对路径的 `cache_dir` 若写在 `initializationOptions` 中，只会分配给第一个文件夹。worker 数量取启动时各文件夹所要求的最大值。列出或切换配置作用于请求所指文件所属的项目——在 VS Code 中即当前活动编辑器的文件——未指定时则作用于第一个文件夹。

## 变量替换

字符串值中支持以下变量：

| 变量           | 说明         |
| -------------- | ------------ |
| `${workspace}` | 项目的根目录 |

## 顶层

顶层选项，写在任何配置节之前。

<!-- BEGIN GENERATED CONFIG: root -->

<div class="config-option">

| 选项                    | 类型     | 默认值 |
| ----------------------- | -------- | ------ |
| `default_configuration` | `string` | `""`   |

当 `--configuration` 与持久化的选择都没有指定配置时，回退使用的构建配置，取规则上声明的标签之一；没有任何规则声明的名称是错误。未设置时使用第一个声明的标签。

</div>

<!-- END GENERATED CONFIG -->

## `[project]`

<!-- BEGIN GENERATED CONFIG: project -->

<div class="config-option">

| 选项        | 类型     | 默认值 |
| ----------- | -------- | ------ |
| `cache_dir` | `string` | `""`   |

统一磁盘缓存的目录（PCH、PCM 和索引产物）。空值默认为 `${workspace}/.clice`，该目录通过生成的 .gitignore 和 CACHEDIR.TAG 标记将自身排除在版本控制和备份之外（`.clice/config.toml` 对 Git 保持可见；遵循 CACHEDIR.TAG 的备份工具会跳过整个目录）；显式配置的目录不会添加这些标记。最终确定的路径会在启动时打印。

</div>

<div class="config-option">

| 选项          | 类型     | 默认值 |
| ------------- | -------- | ------ |
| `logging_dir` | `string` | `""`   |

日志文件目录；空值时使用 `${cache_dir}/logs`。每次服务器会话都将日志写入各自带时间戳的子目录。

</div>

<div class="config-option">

| 选项              | 类型   | 默认值 |
| ----------------- | ------ | ------ |
| `enable_indexing` | `bool` | `true` |

构建后台索引，为跨 TU 功能（查找引用、工作区符号等）提供服务。

</div>

<div class="config-option">

| 选项                              | 类型   | 默认值  |
| --------------------------------- | ------ | ------- |
| `standard_library_instantiations` | `bool` | `false` |

为标准库自身的模板实例化和宏在库内部解析到的目标建立索引——`std::sort` 调用的比较器、`std::vector` 运行的析构函数——让查找引用和调用层次结构能到达这些位置。默认关闭：它们属于库的内部实现，遍历它们要耗费索引时间和空间。修改之后，只对此后索引的翻译单元生效。

</div>

<div class="config-option">

| 选项                 | 类型   | 默认值 |
| -------------------- | ------ | ------ |
| `skip_header_bodies` | `bool` | `true` |

与 clangd 一样，跳过打开的文件在其顶部所包含的头文件中的函数体，但转发实参的模板（`make_unique`）以及 clang 始终会解析的 `constexpr` 和 `auto` 函数除外。文件打开以及编辑后的重建会快上数倍，占用的内存也更少；代价是这些函数体中的错误和警告（其中包括从该文件实例化的头文件模板中的错误和警告），以及声明是否未使用，都只有在后台索引按保存后的内容编译过该文件后才会显示。后台索引以及除 `clice inspect` 之外的所有命令仍会编译这些函数体。

</div>

<div class="config-option">

| 选项       | 类型     | 默认值  |
| ---------- | -------- | ------- |
| `readonly` | `string` | `"off"` |

为打开的文件提供只读服务："off" 以为每个打开的文件构建完整 AST 为目标——构建由第一个需要它的请求触发，期间由索引应答；"on" 不构建 Preamble 的 PCH——读取操作仅由索引提供结果（冷文件会被插到索引队列前端），而代码补全和签名帮助仍会在没有 Preamble 的情况下按需编译；"auto" 使每个文件最初都处于 "on"，在首次出现编辑意图（编辑、代码补全、签名帮助、上下文切换）时切换到 "off"，如果索引无法为某个文件提供服务，则该文件回退到 "off"。功能路由始终选择当前可用的最佳来源进行应答。

</div>

<div class="config-option">

| 选项              | 类型     | 默认值 |
| ----------------- | -------- | ------ |
| `idle_timeout_ms` | `uint32` | `3000` |

后台索引开始前的空闲等待时间，单位毫秒。

</div>

<div class="config-option">

| 选项         | 类型   | 默认值  |
| ------------ | ------ | ------- |
| `test_hooks` | `bool` | `false` |

启用测试框架使用的 clice/internal 测试钩子。

</div>

<div class="config-option">

| 选项                    | 类型     | 默认值 |
| ----------------------- | -------- | ------ |
| `stateful_worker_count` | `uint32` | `2`    |

有状态工作进程的数量——它们在内存中保存 AST 并处理查询（悬停、语义 Token 等）；至少为 1。

</div>

<div class="config-option">

| 选项                     | 类型     | 默认值 |
| ------------------------ | -------- | ------ |
| `stateless_worker_count` | `uint32` | —      |

启动的无状态工作进程数量——它们处理临时任务（PCH/PCM 构建、代码补全、签名帮助、后台索引）——也是后台索引同时占用的工作进程数的上限；默认为机器物理核心数的一半，但至少为 2。设置的值至少为 1。

</div>

<div class="config-option">

| 选项                         | 类型     | 默认值 |
| ---------------------------- | -------- | ------ |
| `min_stateless_worker_count` | `uint32` | `1`    |

无状态工作进程动态伸缩的下限；至少为 1。

</div>

<div class="config-option">

| 选项                         | 类型     | 默认值 |
| ---------------------------- | -------- | ------ |
| `max_stateless_worker_count` | `uint32` | —      |

无状态工作进程动态伸缩的上限，只有在交互式工作发现所有工作进程都在忙时，才会扩容到这一上限；`0` 表示机器的物理核心数，这也是默认值。

</div>

<!-- END GENERATED CONFIG -->

## `[tracker]`

clice 会在后台查看磁盘上的文件，以发现编辑器之外发生的变更（如 `git checkout`、重新生成的 `compile_commands.json`、agent 或代码生成器写出的文件），使服务器无需重启即可检测到这些变更。每个依赖工作区文件的请求在处理之前也会重新查看这些文件；工具链安装的头文件每隔几分钟、在其所在环境更新时以及每次保存时会被重新查看。将 `workspace_poll_seconds` 设为 `0` 可关闭后台轮询。

<!-- BEGIN GENERATED CONFIG: tracker -->

<div class="config-option">

| 选项                     | 类型     | 默认值 |
| ------------------------ | -------- | ------ |
| `workspace_poll_seconds` | `uint32` | `30`   |

工作区文件两次后台查看之间的最长间隔，单位为秒：每次查看发现文件未变化，间隔就翻倍，直到达到这个值。设为 0 将禁用后台轮询，编译数据库的轮询也一并禁用。

</div>

<!-- END GENERATED CONFIG -->

## `[diagnostics]`

`[diagnostics]` 节控制 clice 为已打开文件发布的诊断。

<!-- BEGIN GENERATED CONFIG: diagnostics -->

<div class="config-option">

| 选项         | 类型   | 默认值 |
| ------------ | ------ | ------ |
| `clang_tidy` | `bool` | `true` |

对已打开的文件运行 clang-tidy 并提供它的修复，使用最近的 `.clang-tidy` 配置的检查（没有该文件时使用一小组默认检查）。对编辑器来说过慢的检查，以及在编辑中的代码上不可靠的检查，都不会运行；`clice lint` 不检查的文件不运行 clang-tidy。只有在 clang-tidy 运行时，`NOLINT` 注释才会同时屏蔽编译器警告。

</div>

<div class="config-option">

| 选项       | 类型              | 默认值 |
| ---------- | ----------------- | ------ |
| `suppress` | `array of string` | `[]`   |

按诊断代码列出已打开的文件中不显示的诊断：编译器警告的选项（`-Wunused-variable`）、其他诊断在 clang 中的名称（`err_undeclared_var_use`）、clang-tidy 检查名，或 clice 自己的警告的代码（`toolchain-query-failed`）。适用于 clang 误报的代码：文件的其他功能照常工作，`clice lint` 也仍会报告这些诊断。

</div>

<!-- END GENERATED CONFIG -->

## `[documentation]`

`[documentation]` 节控制 clice 读取文档注释的方式。

<!-- BEGIN GENERATED CONFIG: documentation -->

<div class="config-option">

| 选项     | 类型     | 默认值      |
| -------- | -------- | ----------- |
| `format` | `string` | `"doxygen"` |

文档注释的读取方式，凡是显示文档注释的地方（悬停、代码补全、签名帮助、`clice query`）都按它读取："doxygen" 读取 Doxygen 的命令（`@brief`、`@param`、`@return` 等）以及 Doxygen 允许的 Markdown，同时识别 GTK-Doc 和 kernel-doc 的写法；"plain" 按原样显示注释。

</div>

<div class="config-option">

| 选项      | 类型   | 默认值 |
| --------- | ------ | ------ |
| `inherit` | `bool` | `true` |

重写函数没有自己的文档注释时，显示被重写函数的文档注释。

</div>

<!-- END GENERATED CONFIG -->

## `[hover]`

`[hover]` 节控制悬停卡片的渲染方式。

<!-- BEGIN GENERATED CONFIG: hover -->

<div class="config-option">

| 选项       | 类型   | 默认值 |
| ---------- | ------ | ------ |
| `show_aka` | `bool` | `true` |

显示类型别名所代表的类型，例如“`size_type` is `unsigned long`”。

</div>

<div class="config-option">

| 选项          | 类型     | 默认值 |
| ------------- | -------- | ------ |
| `max_members` | `uint32` | `20`   |

悬停类或枚举时，其定义中最多列出的成员数：类的数据成员和成员类型，或枚举的枚举项；成员函数不列出。`0` 表示只显示声明。

</div>

<div class="config-option">

| 选项     | 类型   | 默认值 |
| -------- | ------ | ------ |
| `layout` | `bool` | `true` |

无论在何处悬停类或字段，都显示编译器为其确定的布局：大小、对齐要求、字段的偏移量，以及填充。

</div>

<!-- END GENERATED CONFIG -->

## `[document_symbols]`

`[document_symbols]` 节控制文件大纲中列出哪些内容。

<!-- BEGIN GENERATED CONFIG: document_symbols -->

<div class="config-option">

| 选项     | 类型   | 默认值 |
| -------- | ------ | ------ |
| `macros` | `bool` | `true` |

列出文件中写出的 `#define`；头文件保护所用的宏始终不列出。

</div>

<div class="config-option">

| 选项     | 类型   | 默认值 |
| -------- | ------ | ------ |
| `locals` | `bool` | `true` |

列出函数体中声明的内容：其中的变量、类型和 Lambda，列在该函数之下。

</div>

<!-- END GENERATED CONFIG -->

## `[inlay_hints]`

`[inlay_hints]` 节控制服务器生成哪些内联提示，以及提示最长可以有多长。修改在重启服务器后生效；此过程不涉及重新编译。

<!-- BEGIN GENERATED CONFIG: inlay_hints -->

<div class="config-option">

| 选项      | 类型   | 默认值 |
| --------- | ------ | ------ |
| `enabled` | `bool` | `true` |

总开关：`false` 会禁用所有内联提示。

</div>

<div class="config-option">

| 选项         | 类型   | 默认值 |
| ------------ | ------ | ------ |
| `parameters` | `bool` | `true` |

调用点的参数名提示，例如 `draw(width: 800, height: 600)`。

</div>

<div class="config-option">

| 选项                 | 类型   | 默认值 |
| -------------------- | ------ | ------ |
| `mutable_references` | `bool` | `true` |

用 `&` 标记通过可变引用传递的实参，例如 `mutate(&value: v)`。

</div>

<div class="config-option">

| 选项             | 类型   | 默认值  |
| ---------------- | ------ | ------- |
| `prefixed_names` | `bool` | `false` |

实参名与去掉单字母前缀后的参数名相同时，仍显示参数名提示，例如传给 `aParam` 的 `param`。

</div>

<div class="config-option">

| 选项                  | 类型   | 默认值  |
| --------------------- | ------ | ------- |
| `single_letter_names` | `bool` | `false` |

函数只有一个参数且参数名为单个字母时，仍显示参数名提示，例如 `push_back(x: 1)`。

</div>

<div class="config-option">

| 选项            | 类型   | 默认值 |
| --------------- | ------ | ------ |
| `deduced_types` | `bool` | `true` |

为 `auto` 变量、结构化绑定（structured bindings）、初始化捕获和 `auto` 参数、`decltype` 所指的类型，以及推导出的返回类型显示推导类型提示。

</div>

<div class="config-option">

| 选项            | 类型   | 默认值  |
| --------------- | ------ | ------- |
| `spelled_types` | `bool` | `false` |

初始化器已写出类型的变量也显示推导类型提示，例如 `static_cast<long>(x)`、`Widget{1, 2}` 或 `std::make_unique<Widget>()`。

</div>

<div class="config-option">

| 选项                         | 类型   | 默认值 |
| ---------------------------- | ------ | ------ |
| `deduced_template_arguments` | `bool` | `true` |

类模板实参推导（class template argument deduction）得出的模板实参，例如 `std::pair pair(1, 2.5)` 中 `pair` 之后的 `<int, double>`。

</div>

<div class="config-option">

| 选项          | 类型   | 默认值 |
| ------------- | ------ | ------ |
| `designators` | `bool` | `true` |

聚合初始化中的字段指派符提示，例如 `Point{1, 2}` 中的 `.x=` 和 `.y=`。

</div>

<div class="config-option">

| 选项            | 类型   | 默认值  |
| --------------- | ------ | ------- |
| `array_indices` | `bool` | `false` |

聚合初始化中的数组索引指派符提示，例如 `int pair[2] = {1, 2}` 中的 `[0]=` 和 `[1]=`。

</div>

<div class="config-option">

| 选项        | 类型   | 默认值  |
| ----------- | ------ | ------- |
| `block_end` | `bool` | `false` |

在函数、类型或控制流代码块的右花括号后显示 `// name`，并链接到代码块的开头。

</div>

<div class="config-option">

| 选项            | 类型   | 默认值 |
| --------------- | ------ | ------ |
| `namespace_end` | `bool` | `true` |

在命名空间的右花括号后显示 `// namespace name`，并链接到命名空间的开头。

</div>

<div class="config-option">

| 选项    | 类型   | 默认值 |
| ------- | ------ | ------ |
| `endif` | `bool` | `true` |

在 `#endif` 后显示 `// condition`，即它所结束的 `#if` 的条件，并链接到该 `#if`。

</div>

<div class="config-option">

| 选项                | 类型   | 默认值  |
| ------------------- | ------ | ------- |
| `default_arguments` | `bool` | `false` |

调用省略的默认实参。

</div>

<div class="config-option">

| 选项              | 类型     | 默认值 |
| ----------------- | -------- | ------ |
| `min_block_lines` | `uint32` | `10`   |

代码块、命名空间或条件编译块在结尾显示提示所需的最小行数，首尾两行都计算在内。

</div>

<div class="config-option">

| 选项               | 类型     | 默认值 |
| ------------------ | -------- | ------ |
| `max_length.types` | `uint32` | `32`   |

推导出的类型或推导出的模板实参的最大长度（字符数）；`0` 表示不设上限。放不下的模板实参会写成 `…`，例如 `tuple<int, string, …>`；连最外层名称都放不下的类型不显示。

</div>

<div class="config-option">

| 选项                           | 类型     | 默认值 |
| ------------------------------ | -------- | ------ |
| `max_length.default_arguments` | `uint32` | `32`   |

省略的默认实参的最大长度（字符数）；`0` 表示不设上限。过长的单个实参会写成 `…`，整个列表在放不下的位置用 `…` 截断。

</div>

<div class="config-option">

| 选项                   | 类型     | 默认值 |
| ---------------------- | -------- | ------ |
| `max_length.block_end` | `uint32` | `60`   |

块结尾或命名空间结尾标签的最大长度（字符数），超出部分用 `…` 截断；`0` 表示不设上限。

</div>

<div class="config-option">

| 选项               | 类型     | 默认值 |
| ------------------ | -------- | ------ |
| `max_length.endif` | `uint32` | `60`   |

`#endif` 条件的最大长度（字符数），超出部分用 `…` 截断；`0` 表示不设上限。

</div>

<!-- END GENERATED CONFIG -->

## `[semantic_tokens]`

`[semantic_tokens]` 节控制语义 Token 包含哪些内容，以及其种类和修饰符以什么名称发送。这些名称在编辑器连接时公布，因此修改在重启服务器后生效。

<!-- BEGIN GENERATED CONFIG: semantic_tokens -->

<div class="config-option">

| 选项      | 类型   | 默认值 |
| --------- | ------ | ------ |
| `lexical` | `bool` | `true` |

同时发送编辑器自身语法高亮也会着色的 Token：关键字、字面量、注释、运算符、预处理指令。关闭时只发送仅凭语法无法区分的内容：按所指实体着色的名称、重载运算符与声明符运算符、模板尖括号、格式说明符，以及预处理非活动代码中的 Token（不带种类）。

</div>

<div class="config-option">

| 选项    | 类型              | 默认值 |
| ------- | ----------------- | ------ |
| `kinds` | `table of string` | `{}`   |

客户端或主题所期望的 clice Token 种类名称，写作 `field = "property"`；名称为空则丢弃该种类，其 Token 不再发送；赋予同一名称的多个种类会合并。在服务器启动时读取。

</div>

<div class="config-option">

| 选项        | 类型              | 默认值 |
| ----------- | ----------------- | ------ |
| `modifiers` | `table of string` | `{}`   |

客户端或主题所期望的 clice Token 修饰符名称；名称为空则丢弃该修饰符，赋予同一名称的多个修饰符会合并。在服务器启动时读取。

</div>

<!-- END GENERATED CONFIG -->

## `[code_completion]`

`[code_completion]` 段控制补全项的组装。

<!-- BEGIN GENERATED CONFIG: code_completion -->

<div class="config-option">

| 选项        | 类型     | 默认值           |
| ----------- | -------- | ---------------- |
| `arguments` | `string` | `"placeholders"` |

补全调用或模板时在名称后插入的内容："placeholders" 插入圆括号或尖括号，并为每个形参放一个占位符；"delimiters" 插入空的圆括号或尖括号，并将光标置于其间；"none" 只插入名称。此设置适用于函数、函数式宏、存放 Lambda 的变量，以及类模板、别名模板和变量模板；合并为一个补全项的重载集只插入空括号。如果实参已经写出，或名称并非用于调用（例如取函数地址），则不插入任何内容。客户端不支持代码片段时，只为调用插入括号。

</div>

<div class="config-option">

| 选项               | 类型   | 默认值 |
| ------------------ | ------ | ------ |
| `bundle_overloads` | `bool` | `true` |

将重载集折叠为单个补全项。

</div>

<div class="config-option">

| 选项                 | 类型   | 默认值  |
| -------------------- | ------ | ------- |
| `statement_snippets` | `bool` | `false` |

以代码片段形式补全 `if`、`for` 等语句，为语句的各个部分提供占位符；否则只插入关键字本身。客户端不支持代码片段时忽略此项。

</div>

<div class="config-option">

| 选项           | 类型   | 默认值 |
| -------------- | ------ | ------ |
| `dot_to_arrow` | `bool` | `true` |

在指针后输入 `.` 时，列出通过 `->` 可访问的成员，并在选中其中一项时把 `.` 改为 `->`。

</div>

<div class="config-option">

| 选项         | 类型   | 默认值 |
| ------------ | ------ | ------ |
| `all_scopes` | `bool` | `true` |

输入非限定名称时，同时提供项目中其他命名空间里与之匹配的名称，并插入所需的限定符；同一名称位于多个命名空间时，每个命名空间各提供一次。当前文件已能访问到的名称按原样提供；其余名称见 `insert_includes` 和 `insert_imports`。当前文件尚无法访问到的名称需要依赖索引。

</div>

<div class="config-option">

| 选项              | 类型   | 默认值 |
| ----------------- | ------ | ------ |
| `insert_includes` | `bool` | `true` |

启用 `all_scopes` 时，同时提供当前文件尚未包含的头文件中声明的名称和定义的宏，并添加它们所在头文件的 `#include`。

</div>

<div class="config-option">

| 选项             | 类型   | 默认值 |
| ---------------- | ------ | ------ |
| `insert_imports` | `bool` | `true` |

启用 `all_scopes` 时，同时提供当前文件尚未导入的模块所导出的名称，并添加相应的 `import`。

</div>

<div class="config-option">

| 选项    | 类型     | 默认值 |
| ------- | -------- | ------ |
| `limit` | `uint32` | `100`  |

补全项的最大数量，只保留排名最靠前的项；`0` 表示不限制。被截断的列表会标记为不完整，因此输入时编辑器会重新请求。

</div>

<!-- END GENERATED CONFIG -->

## `[signature_help]`

`[signature_help]` 节控制签名帮助弹窗显示的内容。

<!-- BEGIN GENERATED CONFIG: signature_help -->

<div class="config-option">

| 选项            | 类型   | 默认值 |
| --------------- | ------ | ------ |
| `documentation` | `bool` | `true` |

显示被调用函数的文档注释。

</div>

<!-- END GENERATED CONFIG -->

## `[references]`

`[references]` 节控制查找引用返回的内容。

<!-- BEGIN GENERATED CONFIG: references -->

<div class="config-option">

| 选项    | 类型     | 默认值 |
| ------- | -------- | ------ |
| `limit` | `uint32` | `2000` |

一次查找引用请求最多返回的位置数；`0` 表示不限制。结果被截断时，优先保留发起请求的文件中的位置。`clice query references` 会列出全部位置。

</div>

<!-- END GENERATED CONFIG -->

## `[[rules]]`

规则通过模式指定文件，并说明这些文件从哪里获取编译命令，以及如何修改这些命令。匹配某个文件的每条规则都会生效，按声明顺序应用：声明在前的规则的数据库在该文件的候选中排在前面，第一条带 `default_command` 的匹配规则为没有条目的文件提供命令，`append` 与 `remove` 逐条累加，靠后的 `remove` 会抵消靠前的 `append`，而任何匹配规则上的 `index = false` 都会把文件排除在索引之外。带 `configuration` 标签的规则只在相应配置生效期间适用；不同的标签构成配置菜单。

<!-- BEGIN GENERATED CONFIG: rules -->

<div class="config-option">

| 选项       | 类型              | 默认值 |
| ---------- | ----------------- | ------ |
| `patterns` | `array of string` | `[]`   |

用于选择此规则所适用文件的 glob 模式。相对模式以本配置文件所在目录为基准（允许 `..` 路径段），通过 initializationOptions 传入的规则则以项目根目录为基准；绝对模式或以 `**` 开头的模式匹配文件的绝对路径。`*` 匹配一个路径段内的任意字符，`?` 匹配单个字符，`**` 匹配任意数量的路径段，`{a,b}` 表示备选项，`[0-9]` 表示字符范围，`[!...]` 表示取反的字符范围。省略时表示所有文件。

</div>

<div class="config-option">

| 选项            | 类型     | 默认值 |
| --------------- | -------- | ------ |
| `configuration` | `string` | `""`   |

构建配置标签。带标签的规则只在相应配置生效期间适用；不带标签的规则始终适用。不同的标签构成配置菜单；`--configuration`、持久化的选择和 `default_configuration` 按此顺序决定哪个配置生效。

</div>

<div class="config-option">

| 选项               | 类型              | 默认值 |
| ------------------ | ----------------- | ------ |
| `compile_commands` | `array of string` | `[]`   |

编译数据库，按优先级排列：可以是 compile_commands.json，也可以是包含它的目录，路径相对于本配置文件（通过 initializationOptions 传入的规则则相对于项目根目录）。它们都会被加载，且无论模式如何，每个条目都适用于其对应的文件；模式和顺序决定同一文件出现在多个数据库中时默认使用哪个条目。不带匹配模式的规则指定的就是整个项目的数据库。当所有规则都没有声明来源时，项目根目录及其一级子目录中的每一份 compile_commands.json 都会被加载，打开某个文件时其上层各级目录中的那些也会被加载。

</div>

<div class="config-option">

| 选项               | 类型   | 默认值  |
| ------------------ | ------ | ------- |
| `resolve_symlinks` | `bool` | `false` |

是否按本规则的 `compile_commands` 经符号链接和 Windows junction 解析出的真实路径来区分构建，而不是按配置名：适用于通过把链接指向某个构建目录来选定构建的构建系统。每个真实路径各有一份索引；链接改指别处时，服务器会请求在那个构建的索引上重启，该索引中仍保留着之前的会话在那里建立的索引。规则与本配置的规则只在直接写明恰好这些数据库这一点上不同的配置，与它共用索引。不再使用的构建的索引会留在缓存目录中，直到被删除。

</div>

<div class="config-option">

| 选项              | 类型                          | 默认值 |
| ----------------- | ----------------------------- | ------ |
| `default_command` | `string` 或 `array of string` | `""`   |

匹配到但没有数据库条目的文件所用的编译命令，其中不含源文件本身：可以是一个按 shell 命令行方式分词的字符串，也可以是 argv 数组。它以读取到该命令的配置文件所在目录为工作目录运行（通过 initializationOptions 传入的规则则以项目根目录为准），磁盘上匹配到的源文件会加入后台索引（除非规则关闭了 `index`）——这些文件在启动时枚举，之后每次工作区轮询时再枚举一遍，因此新建的文件打开时即可编译，并在一个轮询周期内加入索引。clang 不认得语言的文件（Arduino 的 `.ino`、没有扩展名的工具源文件）只有在命令强制了语言（`-x c++`）、且有模式写出了它的后缀（`*.ino`）或它本身时，才算源文件。省略则不提供命令。

</div>

<div class="config-option">

| 选项     | 类型              | 默认值 |
| -------- | ----------------- | ------ |
| `append` | `array of string` | `[]`   |

为匹配的文件追加编译标志，例如 `["-std=c++20", "-DNDEBUG"]`。

</div>

<div class="config-option">

| 选项     | 类型              | 默认值 |
| -------- | ----------------- | ------ |
| `remove` | `array of string` | `[]`   |

为匹配的文件移除编译标志，例如 `["-Wall"]`。带值的标志只移除这个值（`-DDEBUG`），和命令行一样，下一个元素是它的分离值；值写成 `*`（`-I*`、`["-include", "*"]`），或在列表末尾不写值（`["-include"]`）时，移除它的所有值；紧跟在标志名后的 `*` 移除同类标志中所有以这个前缀拼写的（`-W*` 连 `-Wall` 和 `-Wextra` 也一并移除）。

</div>

<div class="config-option">

| 选项              | 类型   | 默认值 |
| ----------------- | ------ | ------ |
| `query_toolchain` | `bool` | `true` |

clice 是否运行匹配文件命令中的编译器，询问它如何编译这些文件：目标、标准库与系统头文件、预定义宏以及默认语言标准。`false` 时不运行任何编译器：由 clice 自带的 clang 按原样读取命令，并采用 clang 针对该编译器名字的默认值。只要有一条匹配的规则写了 `false`，就以它为准。

</div>

<div class="config-option">

| 选项    | 类型   | 默认值 |
| ------- | ------ | ------ |
| `index` | `bool` | `true` |

匹配到的翻译单元是否加入后台索引。`false` 会将它们排除在外；它们被打开时仍会编译，也仍会作为其所包含头文件的宿主。只要有一条匹配的规则写了 `false`，就以它为准。

</div>

<div class="config-option">

| 选项   | 类型   | 默认值 |
| ------ | ------ | ------ |
| `lint` | `bool` | `true` |

`clice lint` 是否检查匹配到的文件。`false` 会将它们排除在外：它们的检查结果会被丢弃，以它们为主文件的翻译单元也不会被解析，除非 `--index` 需要它来构建索引。项目之外的文件从不检查。只要有一条匹配的规则写了 `false`，就以它为准。

</div>

<div class="config-option">

| 选项     | 类型   | 默认值 |
| -------- | ------ | ------ |
| `format` | `bool` | `true` |

`clice format` 是否格式化匹配到的文件。`false` 会将它们排除在外。项目之外的文件从不格式化。只要有一条匹配的规则写了 `false`，就以它为准。

</div>

<!-- END GENERATED CONFIG -->

## 示例

```toml
[[rules]]
compile_commands = ["build"]
append = ["-std=c++23"]

[[rules]]
patterns = ["test/**"]
append = ["-DTEST_MODE"]
```

两个作为可切换配置的构建目录，以及一个没有编译数据库的项目：

```toml
default_configuration = "debug"

[[rules]]
configuration = "debug"
compile_commands = ["build/debug"]

[[rules]]
configuration = "release"
compile_commands = ["build/release"]
```

```toml
[[rules]]
patterns = ["src/**", "include/**"]
default_command = "arm-none-eabi-gcc -std=c23 -mcpu=cortex-m4 -Iinclude"
```

## 规则应用示例

每个示例都展示规则对匹配文件的作用。

<!-- BEGIN GENERATED ITEMS: project/rules -->

<!-- BEGIN CAPABILITY: supported -->

**没有编译数据库的文件**

规则匹配到的文件使用该规则的默认命令编译，之后创建的文件也会加入索引

`src/main.cpp` 使用规则指定的命令编译，不会提示该命令是推测得出的。在服务器运行期间创建的文件会立即以相同方式编译，并在下次扫描磁盘时加入索引。

```case
tests/project/rules/01_default_command
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**按优先级排列的编译数据库**

文件出现在多个编译数据库中时，使用第一个数据库提供的命令，其余命令仍可作为上下文供选择

编译数据库 `a` 为 `main.cpp` 列出的命令带有 `-DFROM_A`；编译数据库 `b` 为它列出的命令带有 `-DFROM_B`，还列出了 `other.cpp`。`main.cpp` 使用 `a` 中的命令编译，也可以切换到 `b` 中的命令，而 `other.cpp` 使用 `b` 中的命令。

```case
tests/project/rules/03_database_order
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**模式以 `clice.toml` 为基准**

相对模式匹配声明它的 `clice.toml` 所在目录下的路径

规则会向 `src/` 下文件的编译命令追加 `-DFEATURE`：`src/main.cpp` 可以编译，而与 `clice.toml` 同目录的 `other.cpp` 缺少该宏。

```case
tests/project/rules/05_relative_patterns
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**从命令中移除标志**

规则会从文件的命令中移除标志，后续规则的移除操作会抵消先前规则的追加操作

编译数据库为 `main.cpp` 提供的命令同时包含 `-DBROKEN` 和 `-include prefix.h`，第一条规则将它们移除，因此该文件可以编译。对于 `other.cpp`，第二条规则追加 `-DFEATURE`，第三条规则又将其移除。

```case
tests/project/rules/09_remove_flags
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->

## 切换配置

规则上的 `configuration` 标签构成一份菜单，每个项目中有一个标签生效；不带标签的规则始终适用。生效的那个按优先级依次是 `--configuration <tag>` 参数（`clice serve`、`clice index`、`clice lint` 和 `clice inspect` 都接受它）、持久化的选择、`default_configuration`。选择在编辑器中完成——在 VS Code 中，C 或 C++ 文件的语言状态（语言模式旁的 `{}`）显示生效的配置并可打开菜单，其他客户端调用 `clice/switchConfiguration`——并存放在 `state.json` 中，该文件位于 `cache_dir` 下，绝不写入 `clice.toml`；它在服务器再次启动时生效，而服务器会请编辑器重启它，与编辑了配置文件时一样。每个配置在 `cache_dir` 下保留各自的索引，因此来回切换绝不会重新索引某个配置已经索引过的内容。批处理命令也通过这些标签选择索引：`clice index --configuration release` 构建 release 配置的索引，`clice index stats` 报告按同样方式解析出的配置所对应的索引。

有些构建系统则是把一个符号链接指向某个构建的目录来选定构建。设置了 `resolve_symlinks = true` 的规则按其 `compile_commands` 经符号链接和 Windows junction 解析出的真实路径来区分构建：链接指向的每个构建各有一份索引；链接改指别处时，运行中的服务器会请求在那个构建的索引上重启；链接再次指回某个构建时，它的索引仍保持离开时的样子。规则只在直接写明该构建的数据库这一点上不同的配置，与它共用索引。索引会一直保留到被删除为止：每份索引都是 `cache_dir` 中 `index` 目录下的一个目录，以配置名和构建路径的最后几级目录命名。

<!-- BEGIN GENERATED ITEMS: project/configurations -->

<!-- BEGIN CAPABILITY: supported -->

**每块板卡一条命令**

带标签的规则组成构建配置菜单，配置切换会在服务器下次启动时生效

每块板卡的规则都指定了各自的默认命令，`board-a` 是默认配置。使用该配置时，头文件中的 `BOARD_B` 分支处于非活动状态；切换到 `board-b` 并重启后，处于非活动状态的则是 `BOARD_A` 分支。

```case
tests/project/configurations/03_board_defaults
```

<!-- END CAPABILITY -->

<!-- BEGIN CAPABILITY: supported -->

**每个链接到的构建一份索引**

解析符号链接的规则为其链接指向过的每个构建各保留一份索引，并在链接改指时跟着切换

构建系统把 `out/current` 指向它选定的构建。指向 `out/debug` 时——`debug` 配置也写明了这个目录——服务器切换到该配置的索引；再指回 `out/release` 时，release 的索引重新提供服务，不会重新索引任何内容。

```case
tests/project/configurations/07_linked_build
```

<!-- END CAPABILITY -->

<!-- END GENERATED ITEMS -->
