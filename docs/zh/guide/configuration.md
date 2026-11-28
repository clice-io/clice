---
outline: 2
---

# 配置参考

本页列出所有选项；常见用法见[配置指南](./config-guide.md)。

clice 从三个文件读取配置，后一个叠加在前一个之上：用户配置文件，即用户配置目录下的 `clice/config.toml`（`$XDG_CONFIG_HOME` 设为绝对路径时为该目录，否则 Linux 与 macOS 上为 `~/.config`，Windows 上为 `%APPDATA%`），作用于每个项目；项目根目录下的 `clice.toml`，若该文件不存在则为 `.clice/config.toml`；以及项目的本地配置文件 `.clice/local.toml`，存放开发者本人的设置，与已提交的 `clice.toml` 并存，默认缓存目录中的 `.gitignore` 会把它排除在 Git 之外（若 `cache_dir` 设在别处，需要自行忽略它）。配置也可以由 LSP 的 `initializationOptions`（JSON 格式）传入，叠加在这三个文件之上。后一层的值会替换前一层的值，表中的各个键分别替换，列表和名称表（`[semantic_tokens.kinds]`）则整体替换。规则则是累加：本地配置文件的规则排在最前，其次是项目的规则，最后是用户配置文件的规则，从而由越具体的文件的规则先决定某个字段；而越具体的文件，其规则的 `append` 和 `remove` 越晚编辑命令，从而让它的编译选项排在最后；`initializationOptions` 中给出的规则会替换所有文件中的规则。合并后仍未设置的项由默认值填充。`clice inspect config` 会列出项目读取的文件，以及这些文件中改变了默认值的每项设置，并标明设置它的文件和行号；`clice command <file>` 会指出决定某个文件每项设置的规则。

配置仅在服务器启动时读取一次，不支持热重载。当项目的配置文件、其本地配置文件或用户配置文件被保存、创建或删除时，服务器会请求编辑器重启自身：VS Code（通过 socket 连接服务器时除外）以及 Neovim 0.12 及更高版本会自动重启它；其他编辑器会在该文件上显示一条诊断（文件被删除时则显示一条消息），说明修改将在 clice 重启后生效。

clice 无法采纳的配置会视为错误：无法解析的文件、类型错误的值、超出选项可选范围的值、为零的 worker 数量或上限、无法编译或含有反斜杠的模式、未设置的变量、没有任何规则标签声明的 `default_configuration`，以及 clice 1.0 之前旧版布局中的键（错误信息会指明它迁移到了何处）。服务器会将每个问题作为错误诊断显示在设置该值的文件上（规则的问题显示在写有该规则的文件上），并在修正后的文件保存之前暂停该项目的所有服务，若问题出在用户配置文件中，则暂停所有项目的服务；命令行运行则会逐一指出每个问题及其位置，并以退出码 2 退出。clice 不认识的键只会在文件上产生一条警告，其余配置照常生效。来自 `initializationOptions` 的设置若有错误，会以消息形式提示，且在修正这些设置并重启 clice 之前不会为该项目提供任何服务。

完整配置的 JSON Schema 发布在 [`clice-config.schema.json`](/clice-config.schema.json)；支持按 Schema 校验 TOML 或 JSON 的编辑器均可指向该地址。VS Code 扩展已为 [Even Better TOML](https://marketplace.visualstudio.com/items?itemName=tamasfe.even-better-toml) 将其关联到每个配置文件，提供补全与校验；在其他环境中，在文件首行写入 `#:schema https://docs.clice.io/clice-config.schema.json` 即可让 taplo（Zed 与 Neovim 配置中常用的 TOML 语言服务器）使用该 Schema。

相对路径和模式均相对于项目根目录解析，无论它们写在哪个配置文件中，还是写在 `initializationOptions` 中：写在用户配置文件中时，它们指向每个项目中的某个位置。

配置项分为两类。作用于整个服务器的选项——缓存与日志目录、检查磁盘的频率、运行的 worker 数量——是顶层键，或位于 `[workers]` 中。可按文件分别设定的选项——后台索引是否编译该文件、是否解析并索引其函数体、如何为其提供服务、做 Lint 和格式化——是 `[[rules]]` 的字段：若要对所有文件生效，请将其写在最后一条不带 `patterns` 的规则上。各功能配置节（`[hover]`、`[inlay_hints]` 等）存放对应功能的偏好设置。

## 多个文件夹

编辑器打开的每个工作区文件夹，只要含有配置文件（`clice.toml`、`.clice/config.toml` 或 `.clice/local.toml`）或 `compile_commands.json`（位于该文件夹本身，或位于其不含自身配置文件的直接子目录中——即自动发现会查找的位置），就是一个独立的项目，各自拥有独立的 `clice.toml`、编译数据库和缓存目录。两者都不含、且位于另一个已打开文件夹内部的文件夹，则归入外层文件夹的项目中。在一个文件夹内部，含有自身 `clice.toml`、`.clice/config.toml` 或 `.clice/local.toml` 的目录自成一个独立项目：外层文件夹的项目不会加载其中的任何编译数据库，也不会把其中的文件当作默认源文件或临时成员；不过若外层文件夹自身的数据库列出了这些文件，仍会在外层项目中编译它们。仅含编译数据库的目录则会作为独立项目，服务外层项目未构建的文件。在所有工作区文件夹之外打开的文件，归属于其上方最近的含有配置文件或 `compile_commands.json`（直接位于该目录或其 `build/` 子目录中）的目录；该目录随后会像已打开的文件夹一样获得服务，除非已有正在服务的项目加载了那份数据库；上方没有此类目录且不被任何项目编译的文件则单独提供服务，使用默认命令，不借用各文件夹项目中的任何内容。这类文件的缓存保存在 clice 的临时目录中，并在服务器退出时删除；从其中某个文件发起的重命名，仅在全部修改都位于该文件之内时才会执行。文件由编译数据库中列有它的项目负责编译；自身没有数据库条目的头文件会从源文件包含了它的项目中借用宿主，并优先选择所在文件夹包含该头文件的项目。工作区符号搜索横跨所有项目；代码导航——查找引用、跳转到定义与声明、调用层次结构与类型层次结构、查找实现——会延伸到在同一文件中声明了该符号的其他项目。编辑器中已打开的文件只由服务它的项目作答，因此所有查询看到的都是它尚未保存的修改。

用户配置文件和 `initializationOptions` 作用于每个项目：其中任一处指定编译数据库或默认命令的规则都会关闭所有项目的数据库自动发现，就像 `clice.toml` 中的此类规则会关闭其所在项目的自动发现一样。一个缓存目录只服务一个项目：位于项目之外的缓存目录会记录最先使用它的项目；其他项目——无论是在当前服务器中，还是在之后的 `clice index` 运行中——都会依次回退到自身 `clice.toml` 或本地配置文件指定的缓存目录、其文件夹内的 `.clice`，并告知用户缓存最终落在了何处；若这些目录也均被占用，该项目将不提供任何服务，直到其 `cache_dir` 指定一个专属于它的目录。因此，这两处填写的绝对路径 `cache_dir` 只会分配给第一个文件夹，除非其中用到了 `${workspace_id}`：`cache_dir = "~/Library/Caches/clice/${workspace_id}"` 会为每个项目分配一个专属目录。worker 数量取启动时各文件夹所请求的最大值。列出或切换构建配置作用于请求所指定文件所属的项目（在 VS Code 中即当前活动编辑器的文件），未指定时则作用于第一个文件夹。

## 日志

`clice serve`、`clice index` 和 `clice lint` 的每次运行都会在 `logging_dir`（默认为项目中的 `.clice/logs`）下创建各自的会话目录，目录名由启动时间和进程 ID 组成：其中包含服务器的 `master.log`（或命令行工具的 `index.log`、`lint.log`），以及每个 worker 各一份日志文件。服务器在打开第一个项目时开始写日志——没有编译数据库的文件夹会等到其中的文件被打开或出现编译数据库时才算打开——之后打开的其他项目都会在各自的日志目录中创建指向该会话日志的链接。每次运行只保留最近的 20 个会话（链接也计入在内），清理其余旧会话，但绝不删除进程仍在运行的会话。单个日志文件超过 64 MiB 时会轮转为 `<name>.1.log`（覆盖已有同名文件），并在新文件开头注明。

无论日志级别如何设置，日志文件始终记录 `info` 及以上级别的内容；环境变量 `CLICE_LOG_LEVEL`（可选 `trace`、`debug`、`info`、`warn`、`error`、`fatal` 或 `off`）控制输出到 stderr 的级别。编辑器会展示服务器的 stderr 输出，默认从 `info` 起（Neovim 配置使用 `warn`）；命令行工具则以 `warning: …` 和 `error: …` 的格式打印警告和错误。设置 `CLICE_LSP_TRACE=1` 后，由编辑器启动的服务器还会把收到（`<-`）和发出（`->`）的每条 LSP 消息连同时间戳写入会话目录下的 `trace.log`，超过 64 MiB 时同样按日志规则轮转。编辑器会将这些环境变量传给它启动的服务器（VS Code 中配置 `clice.serverEnv`）。单个项目中的路径记为相对于项目根目录的路径；同时服务多个项目的服务器记为相对于公共父目录的路径（以文件夹名开头）；worker 自身的日志则写出完整路径。报告问题时请附上整个会话目录：`master.log` 记录了服务器执行的操作及耗时，崩溃时的调用栈保存在崩溃进程对应的日志中；worker 崩溃时，`master.log` 也会摘录其调用栈及最后几行日志。

## 变量替换

`cache_dir`、`logging_dir`，以及规则的 `patterns`、`compile_commands`、`default_command`、`append` 和 `remove` 支持以下变量：

| 变量              | 说明                                                                                                              |
| ----------------- | ----------------------------------------------------------------------------------------------------------------- |
| `${workspace}`    | 项目的根目录                                                                                                      |
| `${workspace_id}` | 项目专属的名称：由根目录名与其路径的哈希组成（`app-1a2b3c4d`），用于为所有项目统一设置的目录                      |
| `${env:NAME}`     | 服务器的环境变量 `NAME`；引用未设置的变量会报错。编辑器会将自身环境传给服务器，VS Code 还会合并 `clice.serverEnv` |

在匹配模式中，变量值按字面字符匹配，绝不当作 glob 语法解析；在写成单个字符串的 `default_command` 中，含空格的变量值仍保持为单个参数。

## 顶层选项

顶层选项，写在所有配置节之前。

clice 会在后台检查磁盘上的文件，以便感知编辑器之外发生的改动（如 `git checkout`、重新生成的 `compile_commands.json`，或 agent、代码生成器写入的文件），无需重启服务器即可同步更新。每个依赖项目文件的请求在处理前也会先检查相应文件；工具链安装的头文件则会每隔几分钟、在其所属环境更新时以及每次保存时检查。将 `poll_seconds` 设为 `0` 可关闭后台轮询。

索引存放在缓存目录中，因此该目录必须位于 clice 可写入的本地磁盘上：若项目的缓存目录位于网络文件系统（NFS、SMB、WSL 下的 Windows 盘符）上或无法写入，该项目将不提供任何服务，clice 会说明原因；此时请将 `cache_dir` 指向本地目录并重启 clice。在 macOS 上，Time Machine 和 iCloud 会备份并同步项目内的缓存目录，将 `cache_dir` 设在 `~/Library/Caches` 下即可避开它们。

<!-- BEGIN GENERATED CONFIG: root -->

<div class="config-option">

| 选项                    | 类型     | 默认值 |
| ----------------------- | -------- | ------ |
| `default_configuration` | `string` | `""`   |

当 `--configuration` 和持久化的选择均未指定配置时回退使用的构建配置，取规则上声明的标签之一；填写没有任何规则声明的名称会报错。未设置时，使用第一个声明的标签。

</div>

<div class="config-option">

| 选项        | 类型     | 默认值 |
| ----------- | -------- | ------ |
| `cache_dir` | `string` | `""`   |

统一磁盘缓存（PCH、PCM 和索引）的目录，相对于项目根目录。留空默认为 `${workspace}/.clice`，该目录会自动生成 .gitignore 和 CACHEDIR.TAG 标记，将自身排除在版本控制和备份之外（`.clice/config.toml` 对 Git 保持可见，`.clice/local.toml` 则不可见；遵循 CACHEDIR.TAG 的备份工具会跳过整个目录）；显式配置的目录不会写入这些标记。一个缓存目录只服务一个项目：在用户配置文件或编辑器设置中为所有项目统一设置的缓存目录，应使用 `${workspace_id}` 为每个项目分配各自的目录。该目录必须是 clice 可写入且位于本地磁盘上的目录：若项目的缓存目录无法写入，或位于网络文件系统（NFS、SMB、WSL 下的 Windows 盘符）上，该项目将不提供任何服务，clice 会说明原因。位于所有工作区文件夹之外的文件，其缓存保存在 clice 的临时目录中，并在服务器退出时删除。解析后的最终路径会在启动时打印。

</div>

<div class="config-option">

| 选项          | 类型     | 默认值 |
| ------------- | -------- | ------ |
| `logging_dir` | `string` | `""`   |

日志文件目录，相对于项目根目录；留空时取 `${cache_dir}/logs`。服务器、`clice index` 和 `clice lint` 的每次运行都会将日志写入各自的子目录，保留最近的 20 个会话。

</div>

<div class="config-option">

| 选项           | 类型     | 默认值 |
| -------------- | -------- | ------ |
| `poll_seconds` | `uint32` | `30`   |

后台两次检查同一项目文件之间的最长间隔（秒）：每次检查若发现文件未变，间隔时间翻倍，直至达到此上限。设为 0 会禁用后台轮询（包括编译数据库的轮询）。

</div>

<!-- END GENERATED CONFIG -->

## `[workers]`

clice 在独立的 worker 进程中执行编译。前台 worker 负责让已打开的文件保持编译状态并响应针对它们的请求；其余 worker 承担一次性任务——后台索引、代码补全、签名帮助、格式化、预编译头文件和模块——并按需启动：始终保留一个常驻待命，后台索引有任务排队时会按可用上限启动 worker，空闲满 30 秒的 worker 则自动退出。同时保留编译状态的已打开文件数量取决于机器内存。因内存不足被系统终止的 worker 会自动重建；以 systemd unit 运行 clice 时需配置 `OOMPolicy=continue`，因为默认策略会在任一子进程被杀时停掉整个 unit（连同服务器本身）。

<!-- BEGIN GENERATED CONFIG: workers -->

<div class="config-option">

| 选项         | 类型     | 默认值 |
| ------------ | -------- | ------ |
| `foreground` | `uint32` | `2`    |

前台 worker 数量：负责让已打开的文件保持编译状态并响应相关请求（悬停、语义 Token 等）；至少为 1。

</div>

<div class="config-option">

| 选项         | 类型                 | 默认值   |
| ------------ | -------------------- | -------- |
| `background` | `"auto"` 或 `uint32` | `"auto"` |

后台索引同时运行的编译任务上限，不会超过 `max`："auto" 表示机器物理核心数的一半（且至少为 2），也可填至少为 1 的整数。在多核机器上调高此值可加快首次索引速度。

</div>

<div class="config-option">

| 选项  | 类型                 | 默认值   |
| ----- | -------------------- | -------- |
| `max` | `"auto"` 或 `uint32` | `"auto"` |

同时运行的编译 worker 总数上限（后台索引、代码补全、签名帮助以及预编译头文件和模块合计）："auto" 表示机器的物理核心数，也可填至少为 1 的整数。这些 worker 按需启动，空闲 30 秒后退出。

</div>

<div class="config-option">

| 选项                  | 类型     | 默认值 |
| --------------------- | -------- | ------ |
| `background_delay_ms` | `uint32` | `3000` |

最后一次编辑或请求之后、后台索引开始运行前的等待时间（毫秒）。

</div>

<!-- END GENERATED CONFIG -->

## `[diagnostics]`

`[diagnostics]` 节控制 clice 为已打开文件发布的诊断。

<!-- BEGIN GENERATED CONFIG: diagnostics -->

<div class="config-option">

| 选项         | 类型   | 默认值 |
| ------------ | ------ | ------ |
| `clang_tidy` | `bool` | `true` |

对已打开的文件运行 clang-tidy 并提供快速修复，使用最近的 `.clang-tidy` 所配置的检查项（未找到配置文件时使用一小组默认检查）。除非将 `slow_clang_tidy_checks` 设为 `true`，否则会跳过对编辑器而言过慢的检查项；在编辑中的代码上不可靠的检查项则始终跳过；`clice lint` 不检查的文件不会运行任何检查。`NOLINT` 注释也仅在启用此项时才会同时抑制编译器警告。

</div>

<div class="config-option">

| 选项                     | 类型   | 默认值 |
| ------------------------ | ------ | ------ |
| `slow_clang_tidy_checks` | `bool` | 未设置 |

对已打开的文件也运行 `.clang-tidy` 中启用但对编辑器而言过慢或尚未测定编辑器耗时的检查项（例如 `misc-const-correctness` 会使大文件的编译耗时翻倍）：文件的每次编译都需等待它们跑完。未设置时，clice 会跳过这些检查项并提示一次它们的名称；设为 `false` 则静默跳过。无论此项如何设置，`clice lint` 都会运行它们。

</div>

<div class="config-option">

| 选项       | 类型              | 默认值 |
| ---------- | ----------------- | ------ |
| `suppress` | `array of string` | `[]`   |

按诊断代码屏蔽已打开文件中的特定诊断：可填编译器警告选项（`-Wunused-variable`）、clang 内部的其他诊断名称（`err_undeclared_var_use`）、clang-tidy 检查项名称，或 clice 自身的警告代码（`toolchain-query-failed`）。适用于处理 clang 误报的代码：文件的其余功能照常工作，且 `clice lint` 仍会报告这些诊断。

</div>

<!-- END GENERATED CONFIG -->

## `[documentation]`

`[documentation]` 节控制 clice 解析文档注释的方式。

<!-- BEGIN GENERATED CONFIG: documentation -->

<div class="config-option">

| 选项     | 类型     | 默认值      |
| -------- | -------- | ----------- |
| `format` | `string` | `"doxygen"` |

凡展示文档注释之处（悬停、代码补全、签名帮助、`clice query`）解析注释的方式："doxygen" 解析 Doxygen 命令（`@brief`、`@param`、`@return` 等）及 Doxygen 支持的 Markdown，同时兼容 GTK-Doc 和 kernel-doc 写法；"plain" 按原文显示注释。

</div>

<div class="config-option">

| 选项      | 类型   | 默认值 |
| --------- | ------ | ------ |
| `inherit` | `bool` | `true` |

重写方法自身没有文档注释时，显示被重写方法的文档注释。

</div>

<!-- END GENERATED CONFIG -->

## `[hover]`

`[hover]` 节控制悬停卡片的渲染内容。

<!-- BEGIN GENERATED CONFIG: hover -->

<div class="config-option">

| 选项       | 类型   | 默认值 |
| ---------- | ------ | ------ |
| `show_aka` | `bool` | `true` |

显示类型别名实际代表的类型，例如 `size_type` 对应 `unsigned long`。

</div>

<div class="config-option">

| 选项          | 类型     | 默认值 |
| ------------- | -------- | ------ |
| `max_members` | `uint32` | `20`   |

悬停在类或枚举上时，其定义中最多列出的成员数量：类的数据成员与成员类型，或枚举的枚举项；成员函数不在此列。设为 `0` 则仅显示声明。

</div>

<div class="config-option">

| 选项     | 类型   | 默认值 |
| -------- | ------ | ------ |
| `layout` | `bool` | `true` |

悬停在任意位置的类或字段上时，显示编译器为其计算的内存布局：大小、对齐、字段偏移量及填充字节。

</div>

<!-- END GENERATED CONFIG -->

## `[document_symbols]`

`[document_symbols]` 节控制文件大纲列出的内容。

<!-- BEGIN GENERATED CONFIG: document_symbols -->

<div class="config-option">

| 选项     | 类型   | 默认值 |
| -------- | ------ | ------ |
| `macros` | `bool` | `true` |

列出文件中编写的 `#define`；头文件保护（include guard）宏始终不列出。

</div>

<div class="config-option">

| 选项     | 类型   | 默认值 |
| -------- | ------ | ------ |
| `locals` | `bool` | `true` |

在函数节点下列出其函数体内声明的变量、局部类型和 Lambda。

</div>

<!-- END GENERATED CONFIG -->

## `[inlay_hints]`

`[inlay_hints]` 节控制服务器生成哪些内联提示及其最大长度。修改在服务器重启后生效。

<!-- BEGIN GENERATED CONFIG: inlay_hints -->

<div class="config-option">

| 选项      | 类型   | 默认值 |
| --------- | ------ | ------ |
| `enabled` | `bool` | `true` |

总开关：设为 `false` 会禁用所有内联提示。

</div>

<div class="config-option">

| 选项         | 类型   | 默认值 |
| ------------ | ------ | ------ |
| `parameters` | `bool` | `true` |

调用处的形参名提示，例如 `draw(width: 800, height: 600)`。

</div>

<div class="config-option">

| 选项                 | 类型   | 默认值 |
| -------------------- | ------ | ------ |
| `mutable_references` | `bool` | `true` |

用 `&` 标记按非 const 引用传递的实参，例如 `mutate(&value: v)`。

</div>

<div class="config-option">

| 选项             | 类型   | 默认值  |
| ---------------- | ------ | ------- |
| `prefixed_names` | `bool` | `false` |

当实参名与形参名去掉单字母前缀后一致时（如将 `param` 传给 `aParam`），是否仍显示形参名提示。

</div>

<div class="config-option">

| 选项                  | 类型   | 默认值  |
| --------------------- | ------ | ------- |
| `single_letter_names` | `bool` | `false` |

单参数函数的形参名为单个字母时，是否仍显示形参名提示，例如 `push_back(x: 1)`。

</div>

<div class="config-option">

| 选项            | 类型   | 默认值 |
| --------------- | ------ | ------ |
| `deduced_types` | `bool` | `true` |

为 `auto` 变量、结构化绑定（structured bindings）、初始化捕获（init-captures）、`auto` 形参、`decltype` 指代的类型以及推导返回类型显示推导类型提示。

</div>

<div class="config-option">

| 选项            | 类型   | 默认值  |
| --------------- | ------ | ------- |
| `spelled_types` | `bool` | `false` |

变量的初始化器中已写出目标类型时（如 `static_cast<long>(x)`、`Widget{1, 2}` 或 `std::make_unique<Widget>()`），是否仍显示推导类型提示。

</div>

<div class="config-option">

| 选项                         | 类型   | 默认值 |
| ---------------------------- | ------ | ------ |
| `deduced_template_arguments` | `bool` | `true` |

显示类模板实参推导（CTAD）得出的模板实参，例如 `std::pair pair(1, 2.5)` 中 `pair` 后的 `<int, double>`。

</div>

<div class="config-option">

| 选项          | 类型   | 默认值 |
| ------------- | ------ | ------ |
| `designators` | `bool` | `true` |

聚合初始化中的成员指派符提示，例如 `Point{1, 2}` 中的 `.x=` 和 `.y=`。

</div>

<div class="config-option">

| 选项            | 类型   | 默认值  |
| --------------- | ------ | ------- |
| `array_indices` | `bool` | `false` |

聚合初始化中的数组下标指派符提示，例如 `int pair[2] = {1, 2}` 中的 `[0]=` 和 `[1]=`。

</div>

<div class="config-option">

| 选项        | 类型   | 默认值  |
| ----------- | ------ | ------- |
| `block_end` | `bool` | `false` |

在函数、类型或控制流代码块的右花括号后显示 `// name`，点击可跳转到块开头。

</div>

<div class="config-option">

| 选项            | 类型   | 默认值 |
| --------------- | ------ | ------ |
| `namespace_end` | `bool` | `true` |

在命名空间的右花括号后显示 `// namespace name`，点击可跳转到命名空间开头。

</div>

<div class="config-option">

| 选项    | 类型   | 默认值 |
| ------- | ------ | ------ |
| `endif` | `bool` | `true` |

在 `#endif` 后显示 `// condition`（即它所闭合的 `#if` 的条件），点击可跳转到对应的 `#if`。

</div>

<div class="config-option">

| 选项                | 类型   | 默认值  |
| ------------------- | ------ | ------- |
| `default_arguments` | `bool` | `false` |

显示调用时省略的默认实参。

</div>

<div class="config-option">

| 选项              | 类型     | 默认值 |
| ----------------- | -------- | ------ |
| `min_block_lines` | `uint32` | `10`   |

代码块、命名空间或条件编译块在末尾显示提示所需的最小行数（首尾两行均计入）。

</div>

<div class="config-option">

| 选项               | 类型                      | 默认值 |
| ------------------ | ------------------------- | ------ |
| `max_length.types` | `"unlimited"` 或 `uint32` | `32`   |

推导类型或推导模板实参的最大字符数，或设为 `"unlimited"` 表示不限长度。放不下的模板实参会缩写为 `…`（如 `tuple<int, string, …>`）；若连最外层类型名都超出上限，则不显示该提示。

</div>

<div class="config-option">

| 选项                           | 类型                      | 默认值 |
| ------------------------------ | ------------------------- | ------ |
| `max_length.default_arguments` | `"unlimited"` 或 `uint32` | `32`   |

所省略默认实参的最大字符数，或设为 `"unlimited"` 表示不限长度。过长的单个实参会缩写为 `…`，整个列表在超出上限处以 `…` 截断。

</div>

<div class="config-option">

| 选项                   | 类型                      | 默认值 |
| ---------------------- | ------------------------- | ------ |
| `max_length.block_end` | `"unlimited"` 或 `uint32` | `60`   |

块结尾或命名空间结尾标签的最大字符数，超出部分以 `…` 截断；也可设为 `"unlimited"` 表示不限长度。

</div>

<div class="config-option">

| 选项               | 类型                      | 默认值 |
| ------------------ | ------------------------- | ------ |
| `max_length.endif` | `"unlimited"` 或 `uint32` | `60`   |

`#endif` 条件提示的最大字符数，超出部分以 `…` 截断；也可设为 `"unlimited"` 表示不限长度。

</div>

<!-- END GENERATED CONFIG -->

## `[semantic_tokens]`

`[semantic_tokens]` 节控制语义 Token 包含的内容，以及发送给客户端的种类（kind）与修饰符（modifier）名称。这些名称在编辑器连接时宣告，因此修改在服务器重启后生效。

<!-- BEGIN GENERATED CONFIG: semantic_tokens -->

<div class="config-option">

| 选项      | 类型   | 默认值 |
| --------- | ------ | ------ |
| `lexical` | `bool` | `true` |

同时发送编辑器自身语法高亮也会着色的 Token：关键字、字面量、注释、运算符、预处理指令。关闭时仅发送单靠语法无法区分的内容：按所指代实体分类的名称、重载运算符与声明符运算符、模板尖括号、格式化说明符，以及预处理非活动代码中的无种类 Token。

</div>

<div class="config-option">

| 选项    | 类型              | 默认值 |
| ------- | ----------------- | ------ |
| `kinds` | `table of string` | `{}`   |

将 clice 的 Token 种类映射为客户端或主题期望的名称，形如 `field = "property"`；映射为空字符串会丢弃该种类（不再发送其 Token），映射到同一名称的多个种类会合并。在服务器启动时读取。

</div>

<div class="config-option">

| 选项        | 类型              | 默认值 |
| ----------- | ----------------- | ------ |
| `modifiers` | `table of string` | `{}`   |

将 clice 的 Token 修饰符映射为客户端或主题期望的名称；映射为空字符串会丢弃该修饰符，映射到同一名称的多个修饰符会合并。在服务器启动时读取。

</div>

<!-- END GENERATED CONFIG -->

## `[code_completion]`

`[code_completion]` 节控制代码补全项的组装方式。

<!-- BEGIN GENERATED CONFIG: code_completion -->

<div class="config-option">

| 选项        | 类型     | 默认值           |
| ----------- | -------- | ---------------- |
| `arguments` | `string` | `"placeholders"` |

补全函数调用或模板时在名称后插入的内容："placeholders" 插入圆括号或尖括号并为每个形参放置占位符；"delimiters" 插入空的圆括号或尖括号并将光标置于中间；"none" 仅插入名称本身。适用于函数、函数式宏、保存 Lambda 的变量，以及类模板、别名模板和变量模板；折叠为单个补全项的重载集只插入空括号。若实参已写出或该名称并非用于调用（如取函数地址），则不额外插入内容。不支持代码片段（snippet）的客户端只会获得函数调用的括号。Objective-C 消息会在选择器的每一段后插入占位符或空 Tab 停靠点；设为 "none" 或客户端不支持代码片段时仅插入第一段。

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

将 `if`、`for` 等语句补全为带各部分占位符的代码片段；关闭时仅插入关键字本身。客户端不支持代码片段时忽略此项。

</div>

<div class="config-option">

| 选项           | 类型   | 默认值 |
| -------------- | ------ | ------ |
| `dot_to_arrow` | `bool` | `true` |

在指针后输入 `.` 时，列出 `->` 可访问的成员，并在选中后自动将 `.` 改为 `->`。

</div>

<div class="config-option">

| 选项         | 类型   | 默认值 |
| ------------ | ------ | ------ |
| `all_scopes` | `bool` | `true` |

输入非限定名称时，同时提供项目中其他命名空间内与之匹配的名称，并自动补上所需的命名空间限定符；位于多个命名空间的同名符号按命名空间分别提供。当前文件已可见的名称直接提供；尚未引入的名称见 `insert_includes` 与 `insert_imports`。其他命名空间中的名称取自索引。

</div>

<div class="config-option">

| 选项              | 类型   | 默认值 |
| ----------------- | ------ | ------ |
| `insert_includes` | `bool` | `true` |

配合 `all_scopes` 使用时，同时提供当前文件尚未包含的头文件中声明的名称与定义的宏，并在选中时自动插入对应头文件的 `#include`。

</div>

<div class="config-option">

| 选项              | 类型              | 默认值 |
| ----------------- | ----------------- | ------ |
| `angled_includes` | `array of string` | `[]`   |

用 glob 模式指定哪些头文件由补全或代码操作添加 `#include` 时使用尖括号。模式匹配写在尖括号之间的路径（`fmt/core.h`），语法与规则的 `patterns` 相同：`fmt/**` 匹配 `fmt/` 下的所有头文件。此项与 `quoted_includes` 都不匹配的头文件，若位于当前文件同目录下或经 `-iquote`、`-I` 找到，使用双引号；若经 `-isystem`、`-idirafter` 或 framework 目录找到，使用尖括号；IWYU pragma 指明应改为包含的头文件，则沿用该 pragma 所写的定界符。经工具链自带目录找到的头文件（标准库的头文件即在其中）始终使用尖括号。即使按模式选定的定界符找不到该头文件，也照模式插入：尖括号不会搜索当前文件所在目录和 `-iquote` 目录。

</div>

<div class="config-option">

| 选项              | 类型              | 默认值 |
| ----------------- | ----------------- | ------ |
| `quoted_includes` | `array of string` | `[]`   |

用 glob 模式指定哪些头文件由补全或代码操作添加 `#include` 时使用双引号，匹配方式见 `angled_includes`。两者都匹配的头文件使用双引号：`angled_includes = ["**"]` 配合 `quoted_includes = ["myproj/**"]`，会让除项目自身头文件以外的所有头文件都使用尖括号。

</div>

<div class="config-option">

| 选项             | 类型   | 默认值 |
| ---------------- | ------ | ------ |
| `insert_imports` | `bool` | `true` |

配合 `all_scopes` 使用时，同时提供当前文件尚未导入的模块所导出的名称，并在选中时自动插入 `import`。若项目构建了 `std` 模块，已导入其他模块的文件或模块单元会用 `import std;` 引入标准库名称，而非包含对应的头文件。

</div>

<div class="config-option">

| 选项    | 类型                      | 默认值 |
| ------- | ------------------------- | ------ |
| `limit` | `"unlimited"` 或 `uint32` | `100`  |

返回补全项的最大数量（保留排序最靠前的项），或设为 `"unlimited"` 表示不限数量。被截断的列表会标记为未完成，编辑器会在继续输入时重新发起请求。

</div>

<!-- END GENERATED CONFIG -->

## `[signature_help]`

`[signature_help]` 节控制签名帮助弹窗的显示内容。

<!-- BEGIN GENERATED CONFIG: signature_help -->

<div class="config-option">

| 选项            | 类型   | 默认值 |
| --------------- | ------ | ------ |
| `documentation` | `bool` | `true` |

显示被调用函数的文档注释。

</div>

<!-- END GENERATED CONFIG -->

## `[references]`

`[references]` 节控制查找引用的返回结果。

<!-- BEGIN GENERATED CONFIG: references -->

<div class="config-option">

| 选项    | 类型                      | 默认值 |
| ------- | ------------------------- | ------ |
| `limit` | `"unlimited"` 或 `uint32` | `2000` |

单次查找引用请求最多返回的位置数量，或设为 `"unlimited"` 表示不限数量。结果被截断时，优先保留发起请求所在文件中的位置。`clice query references` 始终列出全部结果。

</div>

<!-- END GENERATED CONFIG -->

## `[[rules]]`

规则用模式匹配文件，规定这些文件从何处获取编译命令、如何增删命令参数，以及如何索引、服务、Lint 和格式化这些文件。匹配某个文件的所有规则按声明顺序依次应用：靠前规则的编译数据库在该文件的候选中优先级更高；`append` 与 `remove` 逐条累加，靠后的 `remove` 可抵消靠前的 `append`；其余所有字段（`default_command`、`background_index`、`readonly`、`lint` 等）均由第一条设置了该字段的匹配规则决定，若无规则设置则取默认值。因此，针对部分文件的规则应放在针对全部文件的规则之前：不带 `patterns` 的规则若未放在最后，且设置了后续规则也设置的字段，会产生一条警告，因为后续规则的该值永远不会生效。带有 `configuration` 标签的规则仅在该配置处于活动状态时生效；所有不重复的标签共同组成构建配置菜单。运行 `clice command <file>` 可查看指定文件在下方自 `query_toolchain` 起各个字段的最终生效值，以及决定该值的规则。

<!-- BEGIN GENERATED CONFIG: rules -->

<div class="config-option">

| 选项       | 类型              | 默认值 |
| ---------- | ----------------- | ------ |
| `patterns` | `array of string` | `[]`   |

用于筛选本规则适用文件的 glob 模式。相对模式以项目根目录为起点（允许包含 `..` 路径段）；绝对模式或以 `**` 开头的模式匹配文件的绝对路径：解析符号链接后的真实路径，或构建访问该文件所用的路径（例如从别处链接而来的 Bazel `external/...`），匹配其中任一即可。`*` 匹配单个路径段内的任意字符，`?` 匹配单个字符，`**` 匹配任意层级的路径段，`{a,b}` 匹配备选项，`[0-9]` 匹配字符范围，`[!...]` 匹配取反的字符范围，`[*]` 匹配字面字符 `*`。所有平台上的路径分隔符均为 `/`。省略此项表示匹配所有文件：此类规则应放在最后，为上方规则未决定的选项兜底。

</div>

<div class="config-option">

| 选项            | 类型     | 默认值 |
| --------------- | -------- | ------ |
| `configuration` | `string` | `""`   |

构建配置标签。带标签的规则仅在该配置处于活动状态时生效；不带标签的规则始终生效。所有不同的标签共同组成配置菜单；`--configuration`、持久化保存的选择与 `default_configuration` 按此优先级顺序决定当前活动的配置。

</div>

<div class="config-option">

| 选项               | 类型              | 默认值 |
| ------------------ | ----------------- | ------ |
| `compile_commands` | `array of string` | `[]`   |

按优先级排列的编译数据库列表：可填 compile_commands.json 文件路径或包含它的目录路径（相对于项目根目录）。列出的数据库都会加载，且其中每条记录无论规则模式如何都作用于记录自身对应的文件；规则模式与顺序决定了同一文件出现在多个数据库中时默认选用哪条记录。不带模式的规则用于指定整个项目的数据库。当没有任何规则声明来源时，项目根目录及其直接子目录中的所有 compile_commands.json 都会自动加载，打开文件时其上层各级目录中的数据库也会加载；但根目录下含有自身 clice.toml、.clice/config.toml 或 .clice/local.toml 的子目录属于独立项目，其中的数据库不会加载。

</div>

<div class="config-option">

| 选项               | 类型   | 默认值  |
| ------------------ | ------ | ------- |
| `resolve_symlinks` | `bool` | `false` |

是否按本规则的 `compile_commands` 经符号链接和 Windows junction 解析后的真实路径（而非配置名）来区分不同构建：适用于用链接指向当前构建目录的构建系统。每个真实路径各自维护一份索引；当链接改指其他目录时，服务器会请求重启以切换到目标构建的索引，其中仍保留此前会话为该构建建立的数据。若另一配置的规则与当前配置的唯一区别是直接写明了同一批编译数据库路径，则两者共享同一份索引。不再使用的构建索引会保留在缓存目录中，直至手动删除。

</div>

<div class="config-option">

| 选项              | 类型                          | 默认值 |
| ----------------- | ----------------------------- | ------ |
| `default_command` | `string` 或 `array of string` | `""`   |

匹配本规则但没有编译数据库条目的文件所使用的编译命令（不含源文件路径本身）：可为按 shell 命令行规则分词的字符串，或 argv 字符串数组。命令以项目根目录为工作目录运行，磁盘上匹配的源文件会加入后台索引（除非 `background_index` 设为 `false`）——在启动时枚举一次，并在每次轮询时重新枚举，因此后续新建的文件打开时即可编译，并在一个轮询周期内加入索引。clang 无法按扩展名识别语言的文件（如 Arduino 的 `.ino`、无扩展名的工具源码），仅当命令显式指定了语言（如 `-x c++`）且匹配模式点明了其后缀（如 `*.ino`）或文件名本身时，才会视为源文件。省略此项表示不设默认命令。

</div>

<div class="config-option">

| 选项     | 类型              | 默认值 |
| -------- | ----------------- | ------ |
| `append` | `array of string` | `[]`   |

为匹配文件追加的编译选项，例如 `["-std=c++20", "-DNDEBUG"]`。所有匹配规则追加的选项都会生效：先是用户配置文件的，再是项目的，最后是项目本地配置文件的，每个文件内按声明顺序排列，从而让越具体的文件的选项排得越靠后。

</div>

<div class="config-option">

| 选项     | 类型              | 默认值 |
| -------- | ----------------- | ------ |
| `remove` | `array of string` | `[]`   |

为匹配文件移除的编译选项（例如 `["-Wall"]`），在本规则自身的 `append` 之前执行。带值的选项只移除该特定值（如 `-DDEBUG`），列表中的下一个元素与命令行一样视为独立的参数值；将值写为 `*`（如 `-I*`、`["-include", "*"]`）或在列表末尾省略值（如 `["-include"]`）会移除该选项的所有值；紧跟在选项名后的 `*` 则会移除同类选项中所有带这一前缀的项（例如 `-W*` 会同时移除 `-Wall` 和 `-Wextra`）。

</div>

<div class="config-option">

| 选项              | 类型   | 默认值 |
| ----------------- | ------ | ------ |
| `query_toolchain` | `bool` | 未设置 |

clice 是否运行匹配文件编译命令中的编译器，向其查询目标平台、标准库与系统头文件路径、预定义宏及默认语言标准。设为 `false` 时不运行外部编译器，由 clice 内置的 clang 直接按字面读取编译命令，并采用 clang 针对该编译器名称的默认设置。未设置时等同于 `true`。

</div>

<div class="config-option">

| 选项               | 类型   | 默认值 |
| ------------------ | ------ | ------ |
| `background_index` | `bool` | 未设置 |

后台索引是否将匹配文件作为独立的翻译单元编译。设为 `false` 会跳过它们，并清除旧索引中关于它们的记录：打开这些文件时仍会正常编译，其中的头文件也仍会随包含它的翻译单元一起被索引。模块单元或生成预编译头文件的单元仍会在构建相应模块或预编译头文件时顺带建立索引，以供读取它们的文件使用。若不带匹配模式的规则将此项设为 `false`，后台索引将不编译任何文件。未设置时等同于 `true`。

</div>

<div class="config-option">

| 选项                 | 类型   | 默认值 |
| -------------------- | ------ | ------ |
| `skip_function_body` | `bool` | 未设置 |

包含匹配文件的编译是否跳过其函数体。未设置时，编辑器对已打开文件的编译会跳过文件顶部所包含头文件中的函数体——这能让打开与重编速度提升数倍，而这些函数体内部的错误会在后台索引编译完已保存版本后显示——其余所有编译仍会完整读取它们。设为 `true` 时，除匹配文件自身的编译外，其余所有编译（包括后台索引）都会跳过其函数体（含模板函数体），且不对其建立索引——这最适合仅被调用的第三方库代码，能大幅加快索引速度，代价是业务代码触发的库模板内部错误将不再显示；不过转发实参的模板（如 `make_unique`）以及 `constexpr` 和 `auto` 函数的函数体仍会照常读取。设为 `false` 时，任何编译都不跳过函数体，其中的错误会立即显示。

</div>

<div class="config-option">

| 选项                  | 类型   | 默认值 |
| --------------------- | ------ | ------ |
| `index_function_body` | `bool` | 未设置 |

索引是否记录匹配文件函数体内部引用的符号及其模板实例化的解析结果，使查找引用和调用层次结构能覆盖函数体内部。未设置时，对系统头文件为 `false`（系统库自身的内部细节通常只有阅读库源码时才会查询），对其余所有文件为 `true`。在编辑器中打开的文件无论如何都会完整索引。修改此项仅对之后新索引的翻译单元生效。

</div>

<div class="config-option">

| 选项       | 类型     | 默认值 |
| ---------- | -------- | ------ |
| `readonly` | `string` | 未设置 |

匹配文件打开时的服务方式："off" 会编译文件——构建由首个需要编译结果的请求触发，等待期间先由索引回答；"on" 不构建 Preamble 的 PCH——只读查询完全由索引直接响应（尚未索引的文件会排到索引队列最前），而代码补全和签名帮助仍在无 Preamble 的情况下按需编译；"auto" 初始按 "on" 模式打开文件，首次出现编辑意图（修改内容、代码补全、签名帮助、切换上下文）时自动切换为 "off"，若索引无法服务该文件也会回退到 "off"。未设置时等同于 "off"。

</div>

<div class="config-option">

| 选项             | 类型   | 默认值 |
| ---------------- | ------ | ------ |
| `self_contained` | `bool` | 未设置 |

匹配的头文件能否独立编译。`true`：头文件独立编译，不使用包含方的上下文；`false`：始终在包含它的文件上下文中编译。未设置时，clice 会先尝试单独编译一次头文件来自动判定。

</div>

<div class="config-option">

| 选项             | 类型   | 默认值 |
| ---------------- | ------ | ------ |
| `other_contexts` | `bool` | 未设置 |

已打开的匹配文件在内容与索引记录一致期间，是否在当前上下文跳过的代码区域展示其他上下文在该处编译出的信息（索引中保存的语义着色、大纲、折叠范围、文档链接和悬停）。设为 `false` 则仅展示当前上下文自身的内容。编辑器中针对单个文件所做的选择（Show other contexts 或 Hide other contexts）在服务器停止前优先于此设置。未设置时等同于 `true`。

</div>

<div class="config-option">

| 选项   | 类型   | 默认值 |
| ------ | ------ | ------ |
| `lint` | `bool` | 未设置 |

`clice lint` 是否检查匹配文件，以及编辑器是否对它们运行 clang-tidy。设为 `false` 会跳过它们：丢弃其检查结果，且以它们为主文件的翻译单元也不会解析（除非 `--index` 需要为该单元建立索引）。项目目录之外的文件从不检查。未设置时等同于 `true`。

</div>

<div class="config-option">

| 选项     | 类型   | 默认值 |
| -------- | ------ | ------ |
| `format` | `bool` | 未设置 |

`clice format` 是否格式化匹配文件。设为 `false` 会跳过它们。项目目录之外的文件从不格式化。未设置时等同于 `true`。

</div>

<!-- END GENERATED CONFIG -->
