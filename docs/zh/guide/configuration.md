# Configuration

clice 从工作区根目录的 `clice.toml` 中读取配置。配置也可以通过 LSP `initializationOptions`（JSON 格式）传入。

## 变量替换

字符串值中支持以下变量：

| 变量 | 说明 |
|------|------|
| `${workspace}` | 客户端提供的工作区目录 |

## Project

### `project.clang_tidy`

| 类型 | 默认值 |
|------|--------|
| `bool` | `false` |

启用实验性的 clang-tidy 诊断。

### `project.max_active_file`

| 类型 | 默认值 |
|------|--------|
| `int` | `8` |

内存中保持的最大活跃文件数。超过限制时，最近最少使用的文件会被淘汰。范围：1–512。

### `project.cache_dir`

| 类型 | 默认值 |
|------|--------|
| `string` | `$XDG_CACHE_HOME/clice/<hash>` 或 `${workspace}/.clice` |

PCH 和 PCM 缓存文件的存储目录。默认使用 XDG_CACHE_HOME（或 `~/.cache`）下的工作区专用哈希子目录。如果 XDG 目录无法创建，则回退到 `${workspace}/.clice`。

### `project.index_dir`

| 类型 | 默认值 |
|------|--------|
| `string` | `${cache_dir}/index` |

索引文件的存储目录。

### `project.logging_dir`

| 类型 | 默认值 |
|------|--------|
| `string` | `${cache_dir}/logs` |

日志文件目录。

### `project.compile_commands_paths`

| 类型 | 默认值 |
|------|--------|
| `array of string` | `["${workspace}/build"]` |

搜索 `compile_commands.json` 文件的路径。可以是直接的文件路径，也可以是目录（会在其中查找 `compile_commands.json`）。

### `project.enable_indexing`

| 类型 | 默认值 |
|------|--------|
| `bool` | `true` |

启用后台索引，用于跨 TU 功能（查找引用、工作区符号等）。

### `project.idle_timeout_ms`

| 类型 | 默认值 |
|------|--------|
| `int` | `3000` |

最后一次编辑后开始后台索引的空闲等待时间（毫秒）。

### `project.stateful_worker_count`

| 类型 | 默认值 |
|------|--------|
| `uint32` | `2` |

有状态工作进程数量。这些进程在内存中持有 AST，服务查询（hover、semantic tokens 等）。

### `project.stateless_worker_count`

| 类型 | 默认值 |
|------|--------|
| `uint32` | `max(cores/2, 2)` |

无状态工作进程数量。处理临时任务（PCH/PCM 构建、补全、签名帮助）。

### `project.worker_memory_limit`

| 类型 | 默认值 |
|------|--------|
| `uint64` | `4294967296`（4 GB） |

每个工作进程的内存限制（字节）。超过限制的工作进程会被重启。

## Rules

`[[rules]]` 是规则对象数组。规则按声明顺序匹配——后面的规则覆盖前面的。

### `[rules].patterns`

| 类型 | 默认值 |
|------|--------|
| `array of string` | `[]` |

匹配文件路径的 glob 模式：

- `*` — 匹配路径段中的一个或多个字符
- `?` — 匹配路径段中的单个字符
- `**` — 匹配任意数量的路径段，包括零个
- `{}` — 分组条件（如 `**/*.{h,cpp}`）
- `[]` — 字符范围（如 `example.[0-9]`）
- `[!...]` — 排除字符范围

### `[rules].append`

| 类型 | 默认值 |
|------|--------|
| `array of string` | `[]` |

追加到编译命令的标志。例如：`["-std=c++20", "-DNDEBUG"]`。

### `[rules].remove`

| 类型 | 默认值 |
|------|--------|
| `array of string` | `[]` |

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
