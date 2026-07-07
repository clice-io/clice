# Configuration

clice 从工作区根目录的 `clice.toml` 中读取配置；若该文件不存在，则尝试 `.clice/config.toml`。配置也可以通过 LSP `initializationOptions`（JSON 格式）传入：`initializationOptions` 中的值会覆盖配置文件，合并后仍未设置的项再由默认值填充。

配置只在服务器启动时读取一次。修改配置（无论哪个文件）都需要重启服务器，没有热重载。

## 变量替换

字符串值中支持以下变量：

| 变量           | 说明                   |
| -------------- | ---------------------- |
| `${workspace}` | 客户端提供的工作区目录 |

## Project

### `project.clang_tidy`

| 类型   | 默认值  |
| ------ | ------- |
| `bool` | `false` |

启用实验性的 clang-tidy 诊断。**尚未接线**——该选项会被解析，但当前没有任何效果。

### `project.max_active_file`

| 类型  | 默认值 |
| ----- | ------ |
| `int` | `8`    |

内存中保持的最大活跃文件数。**尚未接线**——该选项会被解析，但工作进程仍使用硬编码的上限。

### `project.cache_dir`

| 类型     | 默认值                                                  |
| -------- | ------------------------------------------------------- |
| `string` | `$XDG_CACHE_HOME/clice/<hash>` 或 `${workspace}/.clice` |

统一磁盘缓存的存储目录（PCH、PCM 与索引产物都在这里）。默认使用 XDG_CACHE_HOME（或 `~/.cache`）下的工作区专用哈希子目录。如果 XDG 目录无法创建，则回退到 `${workspace}/.clice`。

### `project.logging_dir`

| 类型     | 默认值              |
| -------- | ------------------- |
| `string` | `${cache_dir}/logs` |

日志文件目录。

### `project.compile_commands_paths`

| 类型              | 默认值 |
| ----------------- | ------ |
| `array of string` | `[]`   |

搜索 `compile_commands.json` 文件的路径。可以是直接的文件路径，也可以是目录（会在其中查找 `compile_commands.json`）。为空（默认）时，clice 会先搜索工作区根目录，再依次搜索其各个直接子目录，使用找到的第一个 `compile_commands.json`。

### `project.enable_indexing`

| 类型   | 默认值 |
| ------ | ------ |
| `bool` | `true` |

启用后台索引，用于跨 TU 功能（查找引用、工作区符号等）。

### `project.idle_timeout_ms`

| 类型  | 默认值 |
| ----- | ------ |
| `int` | `3000` |

最后一次编辑后开始后台索引的空闲等待时间（毫秒）。

### `project.stateful_worker_count`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | `2`    |

有状态工作进程数量。这些进程在内存中持有 AST，服务查询（hover、semantic tokens 等）。

### `project.stateless_worker_count`

| 类型     | 默认值            |
| -------- | ----------------- |
| `uint32` | `max(cores/2, 2)` |

启动时创建的无状态工作进程数量。处理临时任务（PCH/PCM 构建、补全、签名帮助）。

### `project.min_stateless_worker_count`

| 类型     | 默认值      |
| -------- | ----------- |
| `uint32` | `0`（自动） |

无状态工作进程动态缩容的下限。`0` 表示自动确定最小值。

### `project.max_stateless_worker_count`

| 类型     | 默认值      |
| -------- | ----------- |
| `uint32` | `0`（自动） |

无状态工作进程动态扩容的上限。`0` 表示使用 CPU 核心数。

### `project.worker_memory_limit`

| 类型     | 默认值               |
| -------- | -------------------- |
| `uint64` | `4294967296`（4 GB） |

每个工作进程的内存限制（字节）。**尚未强制执行**——该选项会被解析，但基于内存的淘汰/重启尚未实现。

## Tracker

文件追踪器轮询编辑器之外发生的变化（`git checkout`、重新生成的 `compile_commands.json`、代码生成器写出的头文件），使服务器无需重启即可感知。将间隔设为 `0` 可禁用对应的轮询循环。

### `tracker.cdb_poll_seconds`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | `3`    |

重新检查编译数据库文件的间隔（秒）。

### `tracker.workspace_poll_seconds`

| 类型     | 默认值 |
| -------- | ------ |
| `uint32` | `30`   |

扫描工作区文件磁盘变化的间隔（秒）。

## Rules

`[[rules]]` 是规则对象数组。规则按声明顺序匹配——后面的规则覆盖前面的。

### `[rules].patterns`

| 类型              | 默认值 |
| ----------------- | ------ |
| `array of string` | `[]`   |

匹配文件路径的 glob 模式：

- `*` — 匹配路径段中的一个或多个字符
- `?` — 匹配路径段中的单个字符
- `**` — 匹配任意数量的路径段，包括零个
- `{}` — 分组条件（如 `**/*.{h,cpp}`）
- `[]` — 字符范围（如 `example.[0-9]`）
- `[!...]` — 排除字符范围

### `[rules].append`

| 类型              | 默认值 |
| ----------------- | ------ |
| `array of string` | `[]`   |

追加到编译命令的标志。例如：`["-std=c++20", "-DNDEBUG"]`。

### `[rules].remove`

| 类型              | 默认值 |
| ----------------- | ------ |
| `array of string` | `[]`   |

从编译命令中移除的标志。例如：`["-Wall", "-Werror"]`。

## 示例

```toml
[project]
max_active_file = 16
compile_commands_paths = ["${workspace}/build", "${workspace}/cmake-build-debug"]
clang_tidy = true

[[rules]]
patterns = ["**/*"]
append = ["-std=c++23"]

[[rules]]
patterns = ["**/test/**"]
append = ["-DTEST_MODE"]
```
