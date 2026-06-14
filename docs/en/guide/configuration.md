# Configuration

clice reads configuration from `clice.toml` in the workspace root. Configuration can also be passed via LSP `initializationOptions` (JSON format).

## Variable Substitution

The following variable is supported in string values:

| Variable | Description |
|----------|-------------|
| `${workspace}` | The workspace directory provided by the client |

## Project

### `project.clang_tidy`

| Type | Default |
|------|---------|
| `bool` | `false` |

Enable experimental clang-tidy diagnostics.

### `project.max_active_file`

| Type | Default |
|------|---------|
| `int` | `8` |

Maximum number of active files to keep in memory. When the limit is exceeded, the least recently used files are evicted. Range: 1–512.

### `project.cache_dir`

| Type | Default |
|------|---------|
| `string` | `$XDG_CACHE_HOME/clice/<hash>` or `${workspace}/.clice` |

Directory for storing PCH and PCM cache files. The default uses XDG_CACHE_HOME (or `~/.cache`) with a workspace-specific hash subdirectory. Falls back to `${workspace}/.clice` if the XDG directory cannot be created.

### `project.index_dir`

| Type | Default |
|------|---------|
| `string` | `${cache_dir}/index` |

Directory for storing index files.

### `project.logging_dir`

| Type | Default |
|------|---------|
| `string` | `${cache_dir}/logs` |

Directory for log files.

### `project.compile_commands_paths`

| Type | Default |
|------|---------|
| `array of string` | `["${workspace}/build"]` |

Paths to search for `compile_commands.json` files. Can be either direct file paths or directories (will look for `compile_commands.json` inside).

### `project.enable_indexing`

| Type | Default |
|------|---------|
| `bool` | `true` |

Enable background indexing for cross-TU features (find references, workspace symbols, etc.).

### `project.idle_timeout_ms`

| Type | Default |
|------|---------|
| `int` | `3000` |

Idle time (milliseconds) before starting background indexing after the last edit.

### `project.stateful_worker_count`

| Type | Default |
|------|---------|
| `uint32` | `2` |

Number of stateful worker processes. These hold ASTs in memory and serve queries (hover, semantic tokens, etc.).

### `project.stateless_worker_count`

| Type | Default |
|------|---------|
| `uint32` | `max(cores/2, 2)` |

Number of stateless worker processes. These handle ephemeral tasks (PCH/PCM builds, completion, signature help).

### `project.worker_memory_limit`

| Type | Default |
|------|---------|
| `uint64` | `4294967296` (4 GB) |

Per-worker memory limit in bytes. Workers exceeding this limit are restarted.

## Rules

`[[rules]]` is an array of rule objects. Rules are matched in declaration order — later rules override earlier ones.

### `[rules].patterns`

| Type | Default |
|------|---------|
| `array of string` | `[]` |

Glob patterns for matching file paths:

- `*` — matches one or more characters in a path segment
- `?` — matches a single character in a path segment
- `**` — matches any number of path segments, including zero
- `{}` — groups conditions (e.g., `**/*.{h,cpp}`)
- `[]` — character range (e.g., `example.[0-9]`)
- `[!...]` — negated character range

### `[rules].append`

| Type | Default |
|------|---------|
| `array of string` | `[]` |

Flags to append to the compilation command. Example: `["-std=c++20", "-DNDEBUG"]`.

### `[rules].remove`

| Type | Default |
|------|---------|
| `array of string` | `[]` |

Flags to remove from the compilation command. Example: `["-Wall", "-Werror"]`.

## Example

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
