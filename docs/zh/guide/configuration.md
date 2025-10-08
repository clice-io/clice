# Configuration

这是 `clice.toml` 的文档。

## Project

| 名称                | 类型     | 默认值                        |
| ------------------- | -------- | ----------------------------- |
| `project.cache_dir` | `string` | `"${workspace}/.clice/cache"` |

用于储存 PCH 和 PCM 缓存的文件夹。
<br>

| 名称                | 类型     | 默认值                        |
| ------------------- | -------- | ----------------------------- |
| `project.index_dir` | `string` | `"${workspace}/.clice/index"` |

用于储存索引文件的文件夹。
<br>

## Rule

`[[rules]]` 表示一个对象数组，其中每个对象都拥有下面这些属性
<br>

| 名称               | 类型                |
| ------------------ | ------------------- |
| `[rules].patterns` | `array` of `string` |

用于匹配文件路径的 glob patterns，遵循 LSP 的 [标准](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#documentFilter)。

- `*`: 匹配路径段中的一个或多个字符。
- `?`: 匹配路径段中的单个字符。
- `**`: 匹配任意数量的路径段，包括零个。
- `{}`: 用于分组条件 (例如，`**/*.{ts,js}` 匹配所有 TypeScript 和 JavaScript 文件)。
- `[]`: 声明要匹配的路径段中的字符范围 (例如，`example.[0-9]` 匹配 `example.0`, `example.1` 等)。
- `[!...]`: 排除要匹配的路径段中的字符范围 (例如，`example.[!0-9]` 匹配 `example.a`, `example.b`，但不匹配 `example.0`)。
<br>

| 名称             | 类型                | 默认值 |
| ---------------- | ------------------- | ------ |
| `[rules].append` | `array` of `string` | `[]`   |

追加到原始命令列表中的命令。例如，`append = ["-std=c++17"]`。
<br>

| 名称             | 类型                | 默认值 |
| ---------------- | ------------------- | ------ |
| `[rules].remove` | `array` of `string` | `[]`   |

从原始命令列表中移除的命令。例如，`remove = ["-std=c++11"]`。
<br>
