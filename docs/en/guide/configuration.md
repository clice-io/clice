# Configuration

clice reads configuration from `clice.toml` in the workspace root, or from `.clice/config.toml` if the former does not exist. Configuration can also be passed via LSP `initializationOptions` (JSON format); values from `initializationOptions` override the config file, and defaults fill in whatever remains unset after the merge.

Configuration is read once at server startup. Changing it — either file — requires restarting the server; there is no hot reload.

A JSON schema of the whole configuration is published at [`clice-config.schema.json`](/clice-config.schema.json); editors that validate TOML or JSON against a schema can point at it.

## Variable Substitution

The following variable is supported in string values:

| Variable       | Description                                    |
| -------------- | ---------------------------------------------- |
| `${workspace}` | The workspace directory provided by the client |

## Project

<!-- BEGIN GENERATED CONFIG: project -->

### `project.clang_tidy`

| Type   | Default |
| ------ | ------- |
| `bool` | `false` |

Run clang-tidy alongside compiler diagnostics. Not yet wired: the option is parsed but has no effect.

### `project.cache_dir`

| Type     | Default |
| -------- | ------- |
| `string` | `""`    |

Directory for the unified on-disk cache (PCH, PCM and index artifacts). Empty derives it from XDG_CACHE_HOME (or `~/.cache`) with a per-workspace subdirectory named after the workspace plus a short hash, falling back to `${workspace}/.clice`; the resolved path is printed at startup.

### `project.logging_dir`

| Type     | Default |
| -------- | ------- |
| `string` | `""`    |

Directory for log files; empty derives `${cache_dir}/logs`. Each server session logs into its own timestamped subdirectory.

### `project.compile_commands_paths`

| Type              | Default |
| ----------------- | ------- |
| `array of string` | `[]`    |

Paths searched for compile_commands.json — file paths, or directories to look inside. When these all miss — or the list is empty — the workspace root and then each of its immediate subdirectories are searched.

### `project.enable_indexing`

| Type   | Default |
| ------ | ------- |
| `bool` | `true`  |

Build the background index that serves cross-TU features (find references, workspace symbols, ...).

### `project.index_db`

| Type     | Default  |
| -------- | -------- |
| `string` | `"lmdb"` |

Index persistence backend: "lmdb" (single database file) or "files" (one file per blob).

### `project.idle_timeout_ms`

| Type     | Default |
| -------- | ------- |
| `uint32` | `3000`  |

Idle delay in milliseconds before background indexing starts.

### `project.test_hooks`

| Type   | Default |
| ------ | ------- |
| `bool` | `false` |

Enable the clice/internal test hooks used by the test harness.

### `project.stateful_worker_count`

| Type     | Default |
| -------- | ------- |
| `uint32` | `2`     |

Number of stateful workers — they hold ASTs in memory and serve queries (hover, semantic tokens, ...); `0` is invalid and falls back to the default.

### `project.stateless_worker_count`

| Type     | Default |
| -------- | ------- |
| `uint32` | —       |

Initial number of stateless workers — they handle ephemeral tasks (PCH/PCM builds, completion, signature help); defaults to half the machine's parallelism, at least 2. `0` is invalid and falls back to that default.

### `project.min_stateless_worker_count`

| Type     | Default |
| -------- | ------- |
| `uint32` | `1`     |

Lower bound for dynamic stateless-worker scaling; `0` is invalid and falls back to the default.

### `project.max_stateless_worker_count`

| Type     | Default |
| -------- | ------- |
| `uint32` | —       |

Upper bound for dynamic stateless-worker scaling; `0` means the machine's parallelism, which is also the default.

### `project.worker_memory_limit`

| Type     | Default      |
| -------- | ------------ |
| `uint64` | `4294967296` |

Per-stateful-worker memory limit in bytes; `0` is invalid and falls back to the default. Not yet enforced: parsed, but memory-based eviction is not implemented.

<!-- END GENERATED CONFIG -->

## Tracker

The file tracker polls for changes that happen outside the editor (a `git checkout`, a regenerated `compile_commands.json`, code generators writing headers) so the server picks them up without a restart. Setting an interval to `0` disables that polling loop.

<!-- BEGIN GENERATED CONFIG: tracker -->

### `tracker.cdb_poll_seconds`

| Type     | Default |
| -------- | ------- |
| `uint32` | `3`     |

Compilation database poll interval in seconds; 0 disables polling.

### `tracker.workspace_poll_seconds`

| Type     | Default |
| -------- | ------- |
| `uint32` | `30`    |

Workspace file sweep interval in seconds; 0 disables polling.

<!-- END GENERATED CONFIG -->

## Hover

The `[hover]` section controls how hover cards render.

<!-- BEGIN GENERATED CONFIG: hover -->

### `hover.parse_comment_as_markdown`

| Type   | Default |
| ------ | ------- |
| `bool` | `true`  |

Render the hover card as markdown; `false` produces plain text for clients that cannot display it.

### `hover.show_aka`

| Type   | Default |
| ------ | ------- |
| `bool` | `true`  |

Show the desugared form of a type, e.g. `vector<int>::size_type (aka unsigned long)`.

<!-- END GENERATED CONFIG -->

## Inlay Hints

The `[inlay_hints]` section controls which inlay hint categories the server produces. A client-side refresh then requests hints with the updated values; no recompile is involved.

<!-- BEGIN GENERATED CONFIG: inlay_hints -->

### `inlay_hints.enabled`

| Type   | Default |
| ------ | ------- |
| `bool` | `true`  |

Master switch: `false` disables all inlay hints.

### `inlay_hints.parameters`

| Type   | Default |
| ------ | ------- |
| `bool` | `true`  |

Parameter name hints at call sites, e.g. `draw(width: 800, height: 600)`, including `&` markers for arguments passed by mutable reference.

### `inlay_hints.deduced_types`

| Type   | Default |
| ------ | ------- |
| `bool` | `true`  |

Deduced type hints for `auto` variables, structured bindings and deduced return types.

### `inlay_hints.designators`

| Type   | Default |
| ------ | ------- |
| `bool` | `true`  |

Field designator hints in aggregate initialization, e.g. `.x=` and `.y=` in `Point{1, 2}`.

### `inlay_hints.block_end`

| Type   | Default |
| ------ | ------- |
| `bool` | `false` |

`// name` hints after the closing brace of long blocks (functions, types, namespaces, control flow).

### `inlay_hints.default_arguments`

| Type   | Default |
| ------ | ------- |
| `bool` | `false` |

Show the default arguments a call omitted, abbreviated when long.

### `inlay_hints.type_name_limit`

| Type     | Default |
| -------- | ------- |
| `uint32` | `32`    |

Byte budget for rendered hint text: over-long deduced types fall back to a sugared spelling or are dropped, over-long default arguments are abbreviated. `0` means no limit.

<!-- END GENERATED CONFIG -->

## Code Completion

The `[code_completion]` section controls completion item assembly.

<!-- BEGIN GENERATED CONFIG: code_completion -->

### `code_completion.enable_keyword_snippet`

| Type   | Default |
| ------ | ------- |
| `bool` | `false` |

Complete keywords as snippets (not yet implemented).

### `code_completion.enable_function_arguments_snippet`

| Type   | Default |
| ------ | ------- |
| `bool` | `false` |

Insert function arguments as a snippet when completing a call. For functions this applies to individually listed overloads, so it requires `bundle_overloads = false`; function-like macros have no overload sets and always take the snippet.

### `code_completion.enable_template_arguments_snippet`

| Type   | Default |
| ------ | ------- |
| `bool` | `false` |

Insert template arguments as a snippet on completion (not yet implemented).

### `code_completion.insert_paren_in_function_call`

| Type   | Default |
| ------ | ------- |
| `bool` | `false` |

Insert parentheses when completing a function call (not yet implemented).

### `code_completion.bundle_overloads`

| Type   | Default |
| ------ | ------- |
| `bool` | `true`  |

Collapse an overload set into a single completion item.

### `code_completion.limit`

| Type     | Default |
| -------- | ------- |
| `uint32` | `0`     |

Maximum number of completion items (not yet implemented).

<!-- END GENERATED CONFIG -->

## Rules

`[[rules]]` is an array of rule objects. Rules are matched in declaration order — later rules override earlier ones.

<!-- BEGIN GENERATED CONFIG: rules -->

### `[rules].patterns`

| Type              | Default |
| ----------------- | ------- |
| `array of string` | `[]`    |

Glob patterns selecting the files this rule applies to: `*` matches within a path segment (a pattern of just `*` matches any path), `?` a single character, `**` any number of segments, `{a,b}` alternatives, `[0-9]` a character range, `[!...]` a negated range.

### `[rules].append`

| Type              | Default |
| ----------------- | ------- |
| `array of string` | `[]`    |

Compilation flags appended for matching files, e.g. `["-std=c++20", "-DNDEBUG"]`.

### `[rules].remove`

| Type              | Default |
| ----------------- | ------- |
| `array of string` | `[]`    |

Compilation flags removed for matching files, e.g. `["-Wall"]`.

<!-- END GENERATED CONFIG -->

## Example

```toml
[project]
compile_commands_paths = ["${workspace}/build", "${workspace}/cmake-build-debug"]
clang_tidy = true

[[rules]]
patterns = ["**/*"]
append = ["-std=c++23"]

[[rules]]
patterns = ["**/test/**"]
append = ["-DTEST_MODE"]
```
