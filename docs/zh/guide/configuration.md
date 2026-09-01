# Configuration

clice 从工作区根目录的 `clice.toml` 读取配置；若该文件不存在，则从 `.clice/config.toml` 读取。配置也可以通过 LSP `initializationOptions`（JSON 格式）传入；`initializationOptions` 中的值覆盖配置文件，合并后仍未设置的项由默认值填充。

配置只在服务器启动时读取一次。修改配置（无论哪个文件）都需要重启服务器，没有热重载。

完整配置的 JSON schema 发布在 [`clice-config.schema.json`](/clice-config.schema.json)；使用 schema 验证 TOML 或 JSON 的编辑器可以指向它。

## 变量替换

字符串值中支持以下变量：

| 变量           | 说明                   |
| -------------- | ---------------------- |
| `${workspace}` | 客户端提供的工作区目录 |

## Project

<!-- BEGIN GENERATED CONFIG: project -->

### `project.cache_dir`

| 类型     | 默认值 |
| -------- | ------ |
| `string` | `""`   |

统一磁盘缓存的目录（PCH、PCM 和索引产物）。空值默认为 `${workspace}/.clice`，它通过生成的 .gitignore 和 CACHEDIR.TAG 标记将自己排除在版本控制和备份之外（`.clice/config.toml` 对 Git 保持可见；支持 CACHEDIR.TAG 的备份工具会跳过整个目录）；显式配置的目录不会被标记。解析后的路径会在启动时打印。

### `project.logging_dir`

| 类型     | 默认值 |
| -------- | ------ |
| `string` | `""`   |

日志文件目录；空值派生为 `${cache_dir}/logs`。每个服务器会话在各自带有时间戳的子目录中记录日志。

### `project.compile_commands_paths`

| 类型              | 默认值 |
| ----------------- | ------ |
| `array of string` | `[]`   |

搜索 compile_commands.json 的路径——可以是文件路径，也可以是目录（在其中查找）。当这些路径都未命中——或列表为空——则搜索工作区根目录，然后依次搜索其每个直接子目录。

### `project.enable_indexing`

| 类型   | 默认值 |
| ------ | ------ |
| `bool` | `true` |

构建后台索引，为跨 TU 功能（查找引用、工作区符号等）提供服务。

### `project.readonly`

| 类型     | 默认值  |
| -------- | ------- |
| `string` | `"off"` |

打开文件的只读服务：`"off"` 为每个打开文件构建完整 AST——构建由第一个需要它的请求触发，期间由索引应答；`"on"` 从不构建 PCH——读取仅由索引提供（冷文件会插队索引队列），而 completion 和 signature help 仍按需编译，没有 preamble；`"auto"` 让每个文件从 `"on"` 开始，在第一次编辑意图（编辑、completion、signature help、上下文切换）时切换为 `"off"`，且对于索引无法服务的文件回退为 `"off"`。功能路由始终从当前最佳可用来源回答。

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

有状态工作进程的数量——它们在内存中持有 AST 并提供查询服务（hover、semantic tokens 等）；`0` 无效，回退到默认值。

### `project.stateless_worker_count`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | —      |

无状态工作进程的初始数量——它们处理临时任务（PCH/PCM 构建、completion、signature help）；默认为机器并行度的一半，至少为 2。`0` 无效，回退到该默认值。

### `project.min_stateless_worker_count`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | `1`    |

无状态工作进程动态缩容的下限；`0` 无效，回退到默认值。

### `project.max_stateless_worker_count`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | —      |

无状态工作进程动态扩容的上限；`0` 表示机器并行度，这也是默认值。

<!-- END GENERATED CONFIG -->

## Tracker

文件跟踪器轮询编辑器之外发生的变化（如 `git checkout`、重新生成的 `compile_commands.json`、代码生成器写出的头文件），使服务器无需重启即可感知。将间隔设为 `0` 可禁用对应的轮询循环。

<!-- BEGIN GENERATED CONFIG: tracker -->

### `tracker.cdb_poll_seconds`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | `3`    |

编译数据库轮询间隔，单位秒；0 禁用轮询。

### `tracker.workspace_poll_seconds`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | `30`   |

工作区文件扫描间隔，单位秒；0 禁用轮询。

<!-- END GENERATED CONFIG -->

## Hover

`[hover]` 节控制 hover 卡片的渲染方式。

<!-- BEGIN GENERATED CONFIG: hover -->

### `hover.parse_comment_as_markdown`

| 类型   | 默认值 |
| ------ | ------ |
| `bool` | `true` |

将 hover 卡片渲染为 markdown；`false` 时输出纯文本，供无法显示它的客户端使用。

### `hover.show_aka`

| 类型   | 默认值 |
| ------ | ------ |
| `bool` | `true` |

显示类型的去糖形式，例如 `vector<int>::size_type (aka unsigned long)`。

<!-- END GENERATED CONFIG -->

## Inlay Hints

`[inlay_hints]` 段控制服务器生成哪些 inlay hint 类别。客户端刷新时会以更新后的值请求提示；不需要重新编译。

<!-- BEGIN GENERATED CONFIG: inlay_hints -->

### `inlay_hints.enabled`

| 类型   | 默认值 |
| ------ | ------ |
| `bool` | `true` |

总开关：`false` 禁用所有 inlay hints。

### `inlay_hints.parameters`

| 类型   | 默认值 |
| ------ | ------ |
| `bool` | `true` |

调用点的参数名提示，例如 `draw(width: 800, height: 600)`，包括为按可变引用传递的参数添加 `&` 标记。

### `inlay_hints.deduced_types`

| 类型   | 默认值 |
| ------ | ------ |
| `bool` | `true` |

为 `auto` 变量、结构化绑定和推导返回类型显示推导类型提示。

### `inlay_hints.designators`

| 类型   | 默认值 |
| ------ | ------ |
| `bool` | `true` |

聚合初始化中的字段指示符提示，例如 `Point{1, 2}` 中的 `.x=` 和 `.y=`。

### `inlay_hints.block_end`

| 类型   | 默认值  |
| ------ | ------- |
| `bool` | `false` |

长块（函数、类型、命名空间、控制流）的右花括号后显示 `// name` 提示。

### `inlay_hints.default_arguments`

| 类型   | 默认值  |
| ------ | ------- |
| `bool` | `false` |

显示调用省略的默认实参，过长时缩写。

### `inlay_hints.type_name_limit`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | `32`   |

渲染提示文本的字节预算：过长的推导类型回退到糖化拼写或丢弃，过长的默认实参缩写。`0` 表示无限制。

<!-- END GENERATED CONFIG -->

## Code Completion

`[code_completion]` 段控制补全项的组装。

<!-- BEGIN GENERATED CONFIG: code_completion -->

### `code_completion.enable_keyword_snippet`

| 类型   | 默认值  |
| ------ | ------- |
| `bool` | `false` |

将关键字作为 snippet 补全（尚未实现）。

### `code_completion.enable_function_arguments_snippet`

| 类型   | 默认值  |
| ------ | ------- |
| `bool` | `false` |

补全调用时插入函数实参作为 snippet。对函数而言，这适用于单独列出的重载，因此要求 `bundle_overloads = false`；类函数宏没有重载集，总是使用 snippet。

### `code_completion.enable_template_arguments_snippet`

| 类型   | 默认值  |
| ------ | ------- |
| `bool` | `false` |

补全时插入模板实参作为 snippet（尚未实现）。

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

## Rules

`[[rules]]` 是规则对象数组。规则按声明顺序匹配——后面的规则覆盖前面的。

<!-- BEGIN GENERATED CONFIG: rules -->

### `[rules].patterns`

| 类型              | 默认值 |
| ----------------- | ------ |
| `array of string` | `[]`   |

用于选择此规则适用文件的 glob 模式：`*` 匹配路径段内的任意字符（单独一个 `*` 模式匹配任意路径），`?` 匹配单个字符，`**` 匹配任意数量的路径段，`{a,b}` 表示选择项，`[0-9]` 表示字符范围，`[!...]` 表示否定范围。

### `[rules].append`

| 类型              | 默认值 |
| ----------------- | ------ |
| `array of string` | `[]`   |

为匹配文件追加的编译标志，例如 `["-std=c++20", "-DNDEBUG"]`。

### `[rules].remove`

| 类型              | 默认值 |
| ----------------- | ------ |
| `array of string` | `[]`   |

为匹配文件移除的编译标志，例如 `["-Wall"]`。

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
