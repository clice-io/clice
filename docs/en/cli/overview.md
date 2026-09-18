# Command Line Overview

clice is one binary. Editors launch `clice serve` and talk to it over LSP; the other subcommands drive the same engine from a terminal, sharing its compilation database handling, caches and index.

| Command   | What it does                                                              | Page                           |
| --------- | ------------------------------------------------------------------------- | ------------------------------ |
| `serve`   | Start the language server. Editors do this for you, see the editor setup. | [editors](../guide/editors.md) |
| `lint`    | Run clang-tidy over every translation unit with a worker pool.            | [lint](./lint.md)              |
| `index`   | Index a workspace ahead of time so the server starts warm.                | [index](./index.md)            |
| `format`  | Format the workspace's files with clang-format.                           | [format](./format.md)          |
| `inspect` | Run one feature on source files and print the raw result as JSON.         |                                |
| `query`   | Ask the persisted index about symbols, references, call graphs and files. | [query](./query.md)            |
| `doc`     | Extract documentation data from a project. Not implemented yet.           |                                |

`inspect` works today but has no page yet; its options are documented by `clice inspect --help`.

`serve`, `index`, `lint`, `format`, `inspect` and `query` take `--configuration <tag>` to pin the build configuration for the run, over the selection persisted from the editor; see [switching configurations](../guide/configuration.md#switching-configurations).
