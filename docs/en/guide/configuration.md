# Configuration

clice reads configuration from `clice.toml` in the workspace root, or from `.clice/config.toml` if the former does not exist. Configuration can also be passed via LSP `initializationOptions` (JSON format); values from `initializationOptions` override the config file, and defaults fill in whatever remains unset after the merge.

Configuration is read once at server startup. Changing it — either file — requires restarting the server; there is no hot reload.

## Variable Substitution

The following variable is supported in string values:

| Variable       | Description                                    |
| -------------- | ---------------------------------------------- |
| `${workspace}` | The workspace directory provided by the client |

## Project

### `project.clang_tidy`

| Type   | Default |
| ------ | ------- |
| `bool` | `false` |

Enable experimental clang-tidy diagnostics. **Not yet wired** — the option is parsed but has no effect currently.

### `project.max_active_file`

| Type  | Default |
| ----- | ------- |
| `int` | `8`     |

Maximum number of active files to keep in memory. **Not yet wired** — the option is parsed but the worker still uses a hardcoded limit.

### `project.cache_dir`

| Type     | Default                                                             |
| -------- | ------------------------------------------------------------------- |
| `string` | `$XDG_CACHE_HOME/clice/<workspace>-<hash>` or `${workspace}/.clice` |

Directory for the unified on-disk cache (PCH, PCM, and index artifacts all live here). The default uses XDG_CACHE_HOME (or `~/.cache`) with a per-workspace subdirectory named after the workspace directory plus a short hash, e.g. `~/.cache/clice/myproject-1a2b3c4d`. Falls back to `${workspace}/.clice` if the XDG directory cannot be created. The resolved paths are printed at startup in the effective configuration dump (visible in your editor's clice output panel).

### `project.logging_dir`

| Type     | Default             |
| -------- | ------------------- |
| `string` | `${cache_dir}/logs` |

Directory for log files. Each server session logs into its own timestamped subdirectory; the startup log line `Session log directory:` shows the exact path.

### `project.compile_commands_paths`

| Type              | Default |
| ----------------- | ------- |
| `array of string` | `[]`    |

Paths to search for `compile_commands.json` files. Entries can be direct file paths or directories (clice looks for `compile_commands.json` inside). When empty (the default), clice searches the workspace root and then each of its immediate subdirectories, using the first `compile_commands.json` it finds.

### `project.enable_indexing`

| Type   | Default |
| ------ | ------- |
| `bool` | `true`  |

Enable background indexing for cross-TU features (find references, workspace symbols, etc.).

### `project.idle_timeout_ms`

| Type  | Default |
| ----- | ------- |
| `int` | `3000`  |

Idle time (milliseconds) before starting background indexing after the last edit.

### `project.stateful_worker_count`

| Type     | Default |
| -------- | ------- |
| `uint32` | `2`     |

Number of stateful worker processes. These hold ASTs in memory and serve queries (hover, semantic tokens, etc.).

### `project.stateless_worker_count`

| Type     | Default           |
| -------- | ----------------- |
| `uint32` | `max(cores/2, 2)` |

Number of stateless worker processes spawned at startup. These handle ephemeral tasks (PCH/PCM builds, completion, signature help).

### `project.min_stateless_worker_count`

| Type     | Default    |
| -------- | ---------- |
| `uint32` | `0` (auto) |

Lower bound for dynamic scale-down of stateless workers. `0` resolves to an automatic minimum.

### `project.max_stateless_worker_count`

| Type     | Default    |
| -------- | ---------- |
| `uint32` | `0` (auto) |

Upper bound for dynamic scale-up of stateless workers. `0` resolves to the CPU core count.

### `project.worker_memory_limit`

| Type     | Default             |
| -------- | ------------------- |
| `uint64` | `4294967296` (4 GB) |

Per-worker memory limit in bytes. **Not yet enforced** — the option is parsed but memory-based eviction/restart is not implemented yet.

## Tracker

The file tracker polls for changes that happen outside the editor (a `git checkout`, a regenerated `compile_commands.json`, code generators writing headers) so the server picks them up without a restart. Setting an interval to `0` disables that polling loop.

### `tracker.cdb_poll_seconds`

| Type     | Default |
| -------- | ------- |
| `uint32` | `3`     |

Interval for re-checking the compilation database file.

### `tracker.workspace_poll_seconds`

| Type     | Default |
| -------- | ------- |
| `uint32` | `30`    |

Interval for sweeping workspace files for on-disk changes.

## Rules

`[[rules]]` is an array of rule objects. Rules are matched in declaration order — later rules override earlier ones.

### `[rules].patterns`

| Type              | Default |
| ----------------- | ------- |
| `array of string` | `[]`    |

Glob patterns for matching file paths:

- `*` — matches one or more characters in a path segment
- `?` — matches a single character in a path segment
- `**` — matches any number of path segments, including zero
- `{}` — groups conditions (e.g., `**/*.{h,cpp}`)
- `[]` — character range (e.g., `example.[0-9]`)
- `[!...]` — negated character range

### `[rules].append`

| Type              | Default |
| ----------------- | ------- |
| `array of string` | `[]`    |

Flags to append to the compilation command. Example: `["-std=c++20", "-DNDEBUG"]`.

### `[rules].remove`

| Type              | Default |
| ----------------- | ------- |
| `array of string` | `[]`    |

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
