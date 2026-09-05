---
outline: 2
---

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

<div class="config-table">

| Option                       | Type              | Default | Description                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| ---------------------------- | ----------------- | ------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `cache_dir`                  | `string`          | `""`    | Directory for the unified on-disk cache (PCH, PCM and index artifacts). Empty defaults to `${workspace}/.clice`, which keeps itself out of version control and backups via generated .gitignore and CACHEDIR.TAG markers (a `.clice/config.toml` stays visible to Git; backup tools honoring CACHEDIR.TAG skip the whole directory); an explicitly configured directory is never marked. The resolved path is printed at startup.                                                                                                                                                                                                             |
| `logging_dir`                | `string`          | `""`    | Directory for log files; empty derives `${cache_dir}/logs`. Each server session logs into its own timestamped subdirectory.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
| `compile_commands_paths`     | `array of string` | `[]`    | Paths searched for compile_commands.json — file paths, or directories to look inside. When these all miss — or the list is empty — the workspace root and then each of its immediate subdirectories are searched.                                                                                                                                                                                                                                                                                                                                                                                                                             |
| `enable_indexing`            | `bool`            | `true`  | Build the background index that serves cross-TU features (find references, workspace symbols, ...).                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| `readonly`                   | `string`          | `"off"` | Read-only serving for open files: "off" targets a full AST for every open file — builds are pulled by the first request that needs them, with the index answering in the meantime; "on" never builds a PCH — reads serve from the index alone (a cold file jumps the indexing queue), while completion and signature help still compile on demand without a preamble; "auto" starts every file as "on", switches it to "off" at the first edit intent (edit, completion, signature help, context switch), and falls back to "off" for a file the index cannot serve. Feature routing always answers from the best source currently available. |
| `idle_timeout_ms`            | `uint32`          | `3000`  | Idle delay in milliseconds before background indexing starts.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| `test_hooks`                 | `bool`            | `false` | Enable the clice/internal test hooks used by the test harness.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| `stateful_worker_count`      | `uint32`          | `2`     | Number of stateful workers — they hold ASTs in memory and serve queries (hover, semantic tokens, ...); `0` is invalid and falls back to the default.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| `stateless_worker_count`     | `uint32`          | —       | Initial number of stateless workers — they handle ephemeral tasks (PCH/PCM builds, completion, signature help); defaults to half the machine's parallelism, at least 2. `0` is invalid and falls back to that default.                                                                                                                                                                                                                                                                                                                                                                                                                        |
| `min_stateless_worker_count` | `uint32`          | `1`     | Lower bound for dynamic stateless-worker scaling; `0` is invalid and falls back to the default.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               |
| `max_stateless_worker_count` | `uint32`          | —       | Upper bound for dynamic stateless-worker scaling; `0` means the machine's parallelism, which is also the default.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                             |

</div>

<!-- END GENERATED CONFIG -->

## Tracker

The file tracker polls for changes that happen outside the editor (a `git checkout`, a regenerated `compile_commands.json`, code generators writing headers) so the server picks them up without a restart. Setting an interval to `0` disables that polling loop.

<!-- BEGIN GENERATED CONFIG: tracker -->

<div class="config-table">

| Option                   | Type     | Default | Description                                                        |
| ------------------------ | -------- | ------- | ------------------------------------------------------------------ |
| `cdb_poll_seconds`       | `uint32` | `3`     | Compilation database poll interval in seconds; 0 disables polling. |
| `workspace_poll_seconds` | `uint32` | `30`    | Workspace file sweep interval in seconds; 0 disables polling.      |

</div>

<!-- END GENERATED CONFIG -->

## Hover

The `[hover]` section controls how hover cards render.

<!-- BEGIN GENERATED CONFIG: hover -->

<div class="config-table">

| Option                      | Type   | Default | Description                                                                                        |
| --------------------------- | ------ | ------- | -------------------------------------------------------------------------------------------------- |
| `parse_comment_as_markdown` | `bool` | `true`  | Render the hover card as markdown; `false` produces plain text for clients that cannot display it. |
| `show_aka`                  | `bool` | `true`  | Show the desugared form of a type, e.g. `vector<int>::size_type (aka unsigned long)`.              |

</div>

<!-- END GENERATED CONFIG -->

## Inlay Hints

The `[inlay_hints]` section controls which inlay hint categories the server produces. A client-side refresh then requests hints with the updated values; no recompile is involved.

<!-- BEGIN GENERATED CONFIG: inlay_hints -->

<div class="config-table">

| Option              | Type     | Default | Description                                                                                                                                                                  |
| ------------------- | -------- | ------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `enabled`           | `bool`   | `true`  | Master switch: `false` disables all inlay hints.                                                                                                                             |
| `parameters`        | `bool`   | `true`  | Parameter name hints at call sites, e.g. `draw(width: 800, height: 600)`, including `&` markers for arguments passed by mutable reference.                                   |
| `deduced_types`     | `bool`   | `true`  | Deduced type hints for `auto` variables, structured bindings and deduced return types.                                                                                       |
| `designators`       | `bool`   | `true`  | Field designator hints in aggregate initialization, e.g. `.x=` and `.y=` in `Point{1, 2}`.                                                                                   |
| `block_end`         | `bool`   | `false` | `// name` hints after the closing brace of long blocks (functions, types, namespaces, control flow).                                                                         |
| `default_arguments` | `bool`   | `false` | Show the default arguments a call omitted, abbreviated when long.                                                                                                            |
| `type_name_limit`   | `uint32` | `32`    | Byte budget for rendered hint text: over-long deduced types fall back to a sugared spelling or are dropped, over-long default arguments are abbreviated. `0` means no limit. |

</div>

<!-- END GENERATED CONFIG -->

## Code Completion

The `[code_completion]` section controls completion item assembly.

<!-- BEGIN GENERATED CONFIG: code_completion -->

<div class="config-table">

| Option                              | Type     | Default | Description                                                                                                                                                                                                                                    |
| ----------------------------------- | -------- | ------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `enable_keyword_snippet`            | `bool`   | `false` | Complete keywords as snippets (not yet implemented).                                                                                                                                                                                           |
| `enable_function_arguments_snippet` | `bool`   | `false` | Insert function arguments as a snippet when completing a call. For functions this applies to individually listed overloads, so it requires `bundle_overloads = false`; function-like macros have no overload sets and always take the snippet. |
| `enable_template_arguments_snippet` | `bool`   | `false` | Insert template arguments as a snippet on completion (not yet implemented).                                                                                                                                                                    |
| `insert_paren_in_function_call`     | `bool`   | `false` | Insert parentheses when completing a function call (not yet implemented).                                                                                                                                                                      |
| `bundle_overloads`                  | `bool`   | `true`  | Collapse an overload set into a single completion item.                                                                                                                                                                                        |
| `limit`                             | `uint32` | `0`     | Maximum number of completion items (not yet implemented).                                                                                                                                                                                      |

</div>

<!-- END GENERATED CONFIG -->

## Rules

`[[rules]]` is an array of rule objects. Rules are matched in declaration order — later rules override earlier ones.

<!-- BEGIN GENERATED CONFIG: rules -->

<div class="config-table">

| Option     | Type              | Default | Description                                                                                                                                                                                                                                                         |
| ---------- | ----------------- | ------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `patterns` | `array of string` | `[]`    | Glob patterns selecting the files this rule applies to: `*` matches within a path segment (a pattern of just `*` matches any path), `?` a single character, `**` any number of segments, `{a,b}` alternatives, `[0-9]` a character range, `[!...]` a negated range. |
| `append`   | `array of string` | `[]`    | Compilation flags appended for matching files, e.g. `["-std=c++20", "-DNDEBUG"]`.                                                                                                                                                                                   |
| `remove`   | `array of string` | `[]`    | Compilation flags removed for matching files, e.g. `["-Wall"]`.                                                                                                                                                                                                     |

</div>

<!-- END GENERATED CONFIG -->

## Example

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
