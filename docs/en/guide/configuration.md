# Configuration

This is the documentation for `clice.toml`.

## Project

| Name                | Type     | Default                       |
| ------------------- | -------- | ----------------------------- |
| `project.cache_dir` | `string` | `"${workspace}/.clice/cache"` |

Folder for storing PCH and PCM caches.
<br>

| Name                | Type     | Default                       |
| ------------------- | -------- | ----------------------------- |
| `project.index_dir` | `string` | `"${workspace}/.clice/index"` |

Folder for storing index files.
<br>

## Rule

`[[rules]]` represents an array of objects, where each object has the following properties:
<br>

| Name               | Type                |
| ------------------ | ------------------- |
| `[rules].patterns` | `array` of `string` |

Glob patterns for matching file paths, following LSP's [standard](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#documentFilter).

- `*`: Matches one or more characters in a path segment.
- `?`: Matches a single character in a path segment.
- `**`: Matches any number of path segments, including zero.
- `{}`: Used for grouping conditions (e.g., `**/*.{ts,js}` matches all TypeScript and JavaScript files).
- `[]`: Declares a character range to match in a path segment (e.g., `example.[0-9]` matches `example.0`, `example.1`, etc.).
- `[!...]`: Excludes a character range to match in a path segment (e.g., `example.[!0-9]` matches `example.a`, `example.b`, but not `example.0`).
<br>

| Name             | Type                | Default |
| ---------------- | ------------------- | ------- |
| `[rules].append` | `array` of `string` | `[]`    |

Commands to append to the original command list. For example, `append = ["-std=c++17"]`.
<br>

| Name             | Type                | Default |
| ---------------- | ------------------- | ------- |
| `[rules].remove` | `array` of `string` | `[]`    |

Commands to remove from the original command list. For example, `remove = ["-std=c++11"]`.
<br>
