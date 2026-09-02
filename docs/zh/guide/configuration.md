# 配置

clice 从工作区根目录的 `clice.toml` 读取配置；若该文件不存在，则从 `.clice/config.toml` 读取。配置也可以通过 LSP `initializationOptions`（JSON 格式）传入；`initializationOptions` 中的值覆盖配置文件，合并后仍未设置的项由默认值填充。

配置只在服务器启动时读取一次。修改配置（无论哪个文件）都需要重启服务器，没有热重载。

完整配置的 JSON Schema 发布于 [`clice-config.schema.json`](/clice-config.schema.json)；支持根据 Schema 验证 TOML 或 JSON 的编辑器可以使用它。

## 变量替换

字符串值中支持以下变量：

| 变量           | 说明                   |
| -------------- | ---------------------- |
| `${workspace}` | 客户端提供的工作区目录 |

## 项目

<!-- BEGIN GENERATED CONFIG: project -->

### `project.cache_dir`

| 类型     | 默认值 |
| -------- | ------ |
| `string` | `""`   |

统一磁盘缓存的目录（PCH、PCM 和索引产物）。空值默认为 `${workspace}/.clice`，该目录通过生成的 .gitignore 和 CACHEDIR.TAG 标记将自身排除在版本控制和备份之外（`.clice/config.toml` 对 Git 保持可见；遵循 CACHEDIR.TAG 的备份工具会跳过整个目录）；显式配置的目录不会添加这些标记。最终确定的路径会在启动时打印。

### `project.logging_dir`

| 类型     | 默认值 |
| -------- | ------ |
| `string` | `""`   |

日志文件目录；空值时使用 `${cache_dir}/logs`。每次服务器会话都将日志写入各自带时间戳的子目录。

### `project.compile_commands_paths`

| 类型              | 默认值 |
| ----------------- | ------ |
| `array of string` | `[]`   |

搜索 compile_commands.json 的路径——可以是文件路径，也可以是要在其中查找的目录。如果所有指定位置均未找到该文件——或列表为空——则先搜索工作区根目录，再搜索其每个直接子目录。

### `project.enable_indexing`

| 类型   | 默认值 |
| ------ | ------ |
| `bool` | `true` |

构建后台索引，为跨 TU 功能（查找引用、工作区符号等）提供服务。

### `project.readonly`

| 类型     | 默认值  |
| -------- | ------- |
| `string` | `"off"` |

为打开的文件提供只读服务："off" 以为每个打开的文件构建完整 AST 为目标——构建由第一个需要它的请求触发，期间由索引应答；"on" 从不构建 PCH——读取操作仅由索引提供结果（冷文件会被插到索引队列前端），而代码补全和签名帮助仍会在没有 Preamble 的情况下按需编译；"auto" 使每个文件最初都处于 "on"，在首次出现编辑意图（编辑、代码补全、签名帮助、上下文切换）时切换到 "off"，如果索引无法为某个文件提供服务，则该文件回退到 "off"。功能路由始终选择当前可用的最佳来源进行应答。

### `project.idle_timeout_ms`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | `3000` |

后台索引开始前的空闲等待时间，单位毫秒。

### `project.test_hooks`

| 类型   | 默认值  |
| ------ | ------- |
| `bool` | `false` |

启用测试框架使用的 clice/internal 测试钩子。

### `project.stateful_worker_count`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | `2`    |

有状态工作进程的数量——它们在内存中保存 AST 并处理查询（悬停、语义 Token 等）；`0` 无效，回退到默认值。

### `project.stateless_worker_count`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | —      |

无状态工作进程的初始数量——它们处理临时任务（PCH/PCM 构建、代码补全、签名帮助）；默认为机器并行度的一半，但至少为 2。`0` 无效，回退到该默认值。

### `project.min_stateless_worker_count`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | `1`    |

无状态工作进程动态伸缩的下限；`0` 无效，回退到默认值。

### `project.max_stateless_worker_count`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | —      |

无状态工作进程动态伸缩的上限；`0` 表示机器的并行度，这也是默认值。

<!-- END GENERATED CONFIG -->

## 跟踪器

文件跟踪器会轮询编辑器之外发生的变更（如 `git checkout`、重新生成的 `compile_commands.json`、代码生成器写出的头文件），使服务器无需重启即可检测到这些变更。将间隔设为 `0` 可禁用对应的轮询循环。

<!-- BEGIN GENERATED CONFIG: tracker -->

### `tracker.cdb_poll_seconds`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | `3`    |

编译数据库轮询间隔，单位为秒；设为 0 将禁用轮询。

### `tracker.workspace_poll_seconds`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | `30`   |

工作区文件扫描间隔，单位为秒；设为 0 将禁用轮询。

<!-- END GENERATED CONFIG -->

## 悬停

`[hover]` 节控制悬停卡片的渲染方式。

<!-- BEGIN GENERATED CONFIG: hover -->

### `hover.parse_comment_as_markdown`

| 类型   | 默认值 |
| ------ | ------ |
| `bool` | `true` |

将悬停卡片渲染为 Markdown；`false` 时输出纯文本，供无法显示 Markdown 的客户端使用。

### `hover.show_aka`

| 类型   | 默认值 |
| ------ | ------ |
| `bool` | `true` |

显示类型的去糖形式，例如 `vector<int>::size_type (aka unsigned long)`。

<!-- END GENERATED CONFIG -->

## 内联提示

`[inlay_hints]` 节控制服务器生成哪些内联提示类别。客户端刷新后会使用更新后的值请求提示；此过程不涉及重新编译。

<!-- BEGIN GENERATED CONFIG: inlay_hints -->

### `inlay_hints.enabled`

| 类型   | 默认值 |
| ------ | ------ |
| `bool` | `true` |

总开关：`false` 会禁用所有内联提示。

### `inlay_hints.parameters`

| 类型   | 默认值 |
| ------ | ------ |
| `bool` | `true` |

调用点的参数名提示，例如 `draw(width: 800, height: 600)`；以可变引用传递的实参还会带有 `&` 标记。

### `inlay_hints.deduced_types`

| 类型   | 默认值 |
| ------ | ------ |
| `bool` | `true` |

为 `auto` 变量、结构化绑定（structured bindings）和推导出的返回类型显示推导类型提示。

### `inlay_hints.designators`

| 类型   | 默认值 |
| ------ | ------ |
| `bool` | `true` |

聚合初始化中的字段指示符提示，例如 `Point{1, 2}` 中的 `.x=` 和 `.y=`。

### `inlay_hints.block_end`

| 类型   | 默认值  |
| ------ | ------- |
| `bool` | `false` |

在较长代码块（函数、类型、命名空间或控制流结构）的右花括号后显示 `// name` 提示。

### `inlay_hints.default_arguments`

| 类型   | 默认值  |
| ------ | ------- |
| `bool` | `false` |

显示调用省略的默认实参，过长时缩写。

### `inlay_hints.type_name_limit`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | `32`   |

渲染提示文本的字节预算：过长的推导类型会回退为带语法糖的拼写形式或直接省略，过长的默认实参会缩写。`0` 表示不设上限。

<!-- END GENERATED CONFIG -->

## 代码补全

`[code_completion]` 段控制补全项的组装。

<!-- BEGIN GENERATED CONFIG: code_completion -->

### `code_completion.enable_keyword_snippet`

| 类型   | 默认值  |
| ------ | ------- |
| `bool` | `false` |

以代码片段形式补全关键字（尚未实现）。

### `code_completion.enable_function_arguments_snippet`

| 类型   | 默认值  |
| ------ | ------- |
| `bool` | `false` |

补全函数调用时，以代码片段形式插入函数实参。对于函数，此设置适用于逐一列出的重载，因此需要 `bundle_overloads = false`；函数式宏没有重载集，始终使用代码片段。

### `code_completion.enable_template_arguments_snippet`

| 类型   | 默认值  |
| ------ | ------- |
| `bool` | `false` |

补全时以代码片段形式插入模板实参（尚未实现）。

### `code_completion.insert_paren_in_function_call`

| 类型   | 默认值  |
| ------ | ------- |
| `bool` | `false` |

补全函数调用时插入括号（尚未实现）。

### `code_completion.bundle_overloads`

| 类型   | 默认值 |
| ------ | ------ |
| `bool` | `true` |

将重载集折叠为单个补全项。

### `code_completion.limit`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | `0`    |

补全项的最大数量（尚未实现）。

<!-- END GENERATED CONFIG -->

## 规则

`[[rules]]` 是规则对象数组。规则按声明顺序匹配——后面的规则覆盖前面的。

<!-- BEGIN GENERATED CONFIG: rules -->

### `[rules].patterns`

| 类型              | 默认值 |
| ----------------- | ------ |
| `array of string` | `[]`   |

用于选择此规则所适用文件的 glob 模式：`*` 匹配一个路径段内的任意字符（仅包含 `*` 的模式匹配任意路径），`?` 匹配单个字符，`**` 匹配任意数量的路径段，`{a,b}` 表示备选项，`[0-9]` 表示字符范围，`[!...]` 表示取反的字符范围。

### `[rules].append`

| 类型              | 默认值 |
| ----------------- | ------ |
| `array of string` | `[]`   |

为匹配的文件追加编译标志，例如 `["-std=c++20", "-DNDEBUG"]`。

### `[rules].remove`

| 类型              | 默认值 |
| ----------------- | ------ |
| `array of string` | `[]`   |

为匹配的文件移除编译标志，例如 `["-Wall"]`。

<!-- END GENERATED CONFIG -->

## 示例

```toml
[project]
compile_commands_paths = ["${workspace}/build", "${workspace}/cmake-build-debug"]

[[rules]]
patterns = ["**/*"]
append = ["-std=c++23"]

[[rules]]
patterns = ["**/test/**"]
append = ["-DTEST_MODE"]
```
