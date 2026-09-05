---
outline: 2
---

# 配置

clice 从工作区根目录的 `clice.toml` 读取配置；若该文件不存在，则从 `.clice/config.toml` 读取。配置也可以通过 LSP `initializationOptions`（JSON 格式）传入；`initializationOptions` 中的值覆盖配置文件，合并后仍未设置的项由默认值填充。

配置只在服务器启动时读取一次。修改配置（无论哪个文件）都需要重启服务器，没有热重载。

完整配置的 JSON Schema 发布于 [`clice-config.schema.json`](/clice-config.schema.json)；支持根据 Schema 验证 TOML 或 JSON 的编辑器可以使用它。

文件中的相对路径和模式相对于配置文件自身所在的目录解析；通过 `initializationOptions` 传入的值则相对于工作区根目录解析。

## 变量替换

字符串值中支持以下变量：

| 变量           | 说明                   |
| -------------- | ---------------------- |
| `${workspace}` | 客户端提供的工作区目录 |

## 工作区

顶层选项，写在任何配置节之前。

<!-- BEGIN GENERATED CONFIG: root -->

<div class="config-option">

| 选项               | 类型              | 默认值 |
| ------------------ | ----------------- | ------ |
| `compile_commands` | `array of string` | `[]`   |

整个工作区使用的编译数据库，按优先级排列：可以是一个 compile_commands.json，也可以是包含它的目录，路径相对于本配置文件。等价于一条写在最后、不带匹配模式的规则；参见 `[rules].compile_commands`。当这里和所有规则都没有声明来源时，会在工作区根目录及其一级子目录中搜索。

</div>

<div class="config-option">

| 选项                    | 类型     | 默认值 |
| ----------------------- | -------- | ------ |
| `default_configuration` | `string` | `""`   |

启动时生效的构建配置，取规则上声明的标签之一。一旦有任何规则带上标签，此项就是必需的。

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

| 选项       | 类型     | 默认值  |
| ---------- | -------- | ------- |
| `readonly` | `string` | `"off"` |

为打开的文件提供只读服务："off" 以为每个打开的文件构建完整 AST 为目标——构建由第一个需要它的请求触发，期间由索引应答；"on" 从不构建 PCH——读取操作仅由索引提供结果（冷文件会被插到索引队列前端），而代码补全和签名帮助仍会在没有 Preamble 的情况下按需编译；"auto" 使每个文件最初都处于 "on"，在首次出现编辑意图（编辑、代码补全、签名帮助、上下文切换）时切换到 "off"，如果索引无法为某个文件提供服务，则该文件回退到 "off"。功能路由始终选择当前可用的最佳来源进行应答。

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

有状态工作进程的数量——它们在内存中保存 AST 并处理查询（悬停、语义 Token 等）；`0` 无效，回退到默认值。

</div>

<div class="config-option">

| 选项                     | 类型     | 默认值 |
| ------------------------ | -------- | ------ |
| `stateless_worker_count` | `uint32` | —      |

无状态工作进程的初始数量——它们处理临时任务（PCH/PCM 构建、代码补全、签名帮助）；默认为机器并行度的一半，但至少为 2。`0` 无效，回退到该默认值。

</div>

<div class="config-option">

| 选项                         | 类型     | 默认值 |
| ---------------------------- | -------- | ------ |
| `min_stateless_worker_count` | `uint32` | `1`    |

无状态工作进程动态伸缩的下限；`0` 无效，回退到默认值。

</div>

<div class="config-option">

| 选项                         | 类型     | 默认值 |
| ---------------------------- | -------- | ------ |
| `max_stateless_worker_count` | `uint32` | —      |

无状态工作进程动态伸缩的上限；`0` 表示机器的并行度，这也是默认值。

</div>

<!-- END GENERATED CONFIG -->

## `[tracker]`

文件跟踪器会轮询编辑器之外发生的变更（如 `git checkout`、重新生成的 `compile_commands.json`、代码生成器写出的头文件），使服务器无需重启即可检测到这些变更。将间隔设为 `0` 可禁用对应的轮询循环。

<!-- BEGIN GENERATED CONFIG: tracker -->

<div class="config-option">

| 选项               | 类型     | 默认值 |
| ------------------ | -------- | ------ |
| `cdb_poll_seconds` | `uint32` | `3`    |

编译数据库轮询间隔，单位为秒；设为 0 将禁用轮询。

</div>

<div class="config-option">

| 选项                     | 类型     | 默认值 |
| ------------------------ | -------- | ------ |
| `workspace_poll_seconds` | `uint32` | `30`   |

工作区文件扫描间隔，单位为秒；设为 0 将禁用轮询。

</div>

<!-- END GENERATED CONFIG -->

## `[hover]`

`[hover]` 节控制悬停卡片的渲染方式。

<!-- BEGIN GENERATED CONFIG: hover -->

<div class="config-option">

| 选项                        | 类型   | 默认值 |
| --------------------------- | ------ | ------ |
| `parse_comment_as_markdown` | `bool` | `true` |

将悬停卡片渲染为 Markdown；`false` 时输出纯文本，供无法显示 Markdown 的客户端使用。

</div>

<div class="config-option">

| 选项       | 类型   | 默认值 |
| ---------- | ------ | ------ |
| `show_aka` | `bool` | `true` |

显示类型的去糖形式，例如 `vector<int>::size_type (aka unsigned long)`。

</div>

<!-- END GENERATED CONFIG -->

## `[inlay_hints]`

`[inlay_hints]` 节控制服务器生成哪些内联提示类别。客户端刷新后会使用更新后的值请求提示；此过程不涉及重新编译。

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

调用点的参数名提示，例如 `draw(width: 800, height: 600)`；以可变引用传递的实参还会带有 `&` 标记。

</div>

<div class="config-option">

| 选项            | 类型   | 默认值 |
| --------------- | ------ | ------ |
| `deduced_types` | `bool` | `true` |

为 `auto` 变量、结构化绑定（structured bindings）和推导出的返回类型显示推导类型提示。

</div>

<div class="config-option">

| 选项          | 类型   | 默认值 |
| ------------- | ------ | ------ |
| `designators` | `bool` | `true` |

聚合初始化中的字段指示符提示，例如 `Point{1, 2}` 中的 `.x=` 和 `.y=`。

</div>

<div class="config-option">

| 选项        | 类型   | 默认值  |
| ----------- | ------ | ------- |
| `block_end` | `bool` | `false` |

在较长代码块（函数、类型、命名空间或控制流结构）的右花括号后显示 `// name` 提示。

</div>

<div class="config-option">

| 选项                | 类型   | 默认值  |
| ------------------- | ------ | ------- |
| `default_arguments` | `bool` | `false` |

显示调用省略的默认实参，过长时缩写。

</div>

<div class="config-option">

| 选项              | 类型     | 默认值 |
| ----------------- | -------- | ------ |
| `type_name_limit` | `uint32` | `32`   |

渲染提示文本的字节预算：过长的推导类型会回退为带语法糖的拼写形式或直接省略，过长的默认实参会缩写。`0` 表示不设上限。

</div>

<!-- END GENERATED CONFIG -->

## `[code_completion]`

`[code_completion]` 段控制补全项的组装。

<!-- BEGIN GENERATED CONFIG: code_completion -->

<div class="config-option">

| 选项                     | 类型   | 默认值  |
| ------------------------ | ------ | ------- |
| `enable_keyword_snippet` | `bool` | `false` |

以代码片段形式补全关键字（尚未实现）。

</div>

<div class="config-option">

| 选项                                | 类型   | 默认值  |
| ----------------------------------- | ------ | ------- |
| `enable_function_arguments_snippet` | `bool` | `false` |

补全函数调用时，以代码片段形式插入函数实参。对于函数，此设置适用于逐一列出的重载，因此需要 `bundle_overloads = false`；函数式宏没有重载集，始终使用代码片段。

</div>

<div class="config-option">

| 选项                                | 类型   | 默认值  |
| ----------------------------------- | ------ | ------- |
| `enable_template_arguments_snippet` | `bool` | `false` |

补全时以代码片段形式插入模板实参（尚未实现）。

</div>

<div class="config-option">

| 选项                            | 类型   | 默认值  |
| ------------------------------- | ------ | ------- |
| `insert_paren_in_function_call` | `bool` | `false` |

补全函数调用时插入括号（尚未实现）。

</div>

<div class="config-option">

| 选项               | 类型   | 默认值 |
| ------------------ | ------ | ------ |
| `bundle_overloads` | `bool` | `true` |

将重载集折叠为单个补全项。

</div>

<div class="config-option">

| 选项    | 类型     | 默认值 |
| ------- | -------- | ------ |
| `limit` | `uint32` | `0`    |

补全项的最大数量（尚未实现）。

</div>

<!-- END GENERATED CONFIG -->

## `[[rules]]`

规则通过模式指定文件，并说明这些文件从哪里获取编译命令，以及如何修改这些命令。规则按声明顺序匹配——后声明的规则覆盖先声明的规则。带 `configuration` 标签的规则只在相应配置生效期间适用；不同的标签构成配置菜单。

<!-- BEGIN GENERATED CONFIG: rules -->

<div class="config-option">

| 选项       | 类型              | 默认值 |
| ---------- | ----------------- | ------ |
| `patterns` | `array of string` | `[]`   |

用于选择此规则所适用文件的 glob 模式。相对模式以本配置文件所在目录为基准（允许 `..` 路径段）；绝对模式或以 `**` 开头的模式匹配文件的绝对路径。`*` 匹配一个路径段内的任意字符（仅包含 `*` 的模式匹配任意路径），`?` 匹配单个字符，`**` 匹配任意数量的路径段，`{a,b}` 表示备选项，`[0-9]` 表示字符范围，`[!...]` 表示取反的字符范围。省略时表示所有文件。

</div>

<div class="config-option">

| 选项            | 类型     | 默认值 |
| --------------- | -------- | ------ |
| `configuration` | `string` | `""`   |

构建配置标签。带标签的规则只在相应配置生效期间适用；不带标签的规则始终适用。不同的标签构成配置菜单，`default_configuration` 指定启动时生效的配置。

</div>

<div class="config-option">

| 选项               | 类型              | 默认值 |
| ------------------ | ----------------- | ------ |
| `compile_commands` | `array of string` | `[]`   |

编译数据库，按优先级排列：可以是 compile_commands.json，也可以是包含它的目录，路径相对于本配置文件。它们都会被加载，且无论模式如何，每个条目都适用于其对应的文件；模式和顺序决定同一文件出现在多个数据库中时默认使用哪个条目。

</div>

<div class="config-option">

| 选项              | 类型                          | 默认值 |
| ----------------- | ----------------------------- | ------ |
| `default_command` | `string` 或 `array of string` | `""`   |

匹配到但没有数据库条目的文件所用的编译命令，其中不含源文件本身：可以是一个按 shell 命令行方式分词的字符串，也可以是 argv 数组。它以本配置文件所在目录为工作目录运行，磁盘上匹配到的源文件会加入后台索引。省略则不提供命令。

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

为匹配的文件移除编译标志，例如 `["-Wall"]`。

</div>

<div class="config-option">

| 选项    | 类型   | 默认值 |
| ------- | ------ | ------ |
| `index` | `bool` | `true` |

匹配到的翻译单元是否加入后台索引。`false` 会将它们排除在外；它们被打开时仍会编译，也仍会作为其所包含头文件的宿主。只要有一条匹配的规则写了 `false`，就以它为准。

</div>

<!-- END GENERATED CONFIG -->

## 示例

```toml
compile_commands = ["build"]

[[rules]]
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
