# Command Line Overview

clice is one binary. Editors launch `clice serve` and talk to it over LSP; the other subcommands drive the same engine from a terminal, sharing its compilation database handling, caches and index.

| Command   | What it does                                                              | Page                           |
| --------- | ------------------------------------------------------------------------- | ------------------------------ |
| `serve`   | Start the language server. Editors do this for you, see the editor setup. | [editors](../guide/editors.md) |
| `lint`    | Run clang-tidy over every translation unit with a worker pool.            | [lint](./lint.md)              |
| `index`   | Index a workspace ahead of time so the server starts warm.                |                                |
| `format`  | Format C++ source files. Not implemented yet.                             |                                |
| `inspect` | Run one feature on source files and print the raw result as JSON.         |                                |
| `query`   | Query symbol information from a running server.                           |                                |
| `doc`     | Extract documentation data from a project. Not implemented yet.           |                                |

`index`, `inspect` and `query` work today but have no page yet; their options are documented by `clice <command> --help`.
