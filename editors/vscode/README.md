# clice

C++ language support powered by [clice](https://github.com/clice-io/clice), a
language server written from scratch on LLVM/Clang: template-aware completion
inside generic code, first-class compilation contexts for headers and build
configurations, and native C++20 modules support.

## Getting started

1. Install this extension — the clice server for your platform is bundled, no
   download or extra setup needed.
2. Open a C++ project with a
   [compilation database](https://clang.llvm.org/docs/JSONCompilationDatabase.html).
   For CMake: `cmake -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`. clice
   searches the workspace root and its immediate subdirectories (e.g.
   `build/`) automatically.

See the [documentation](https://docs.clice.io/clice/) for configuration via
`clice.toml` and the full feature guide.

## Release channels

Stable versions use even minor numbers (`0.2.x`); daily pre-releases from
`main` use odd ones (`0.1.x`). Use `Switch to Pre-Release Version` on the
extension page to follow the pre-release channel.

## Settings

| Setting            | Default     | Description                                                            |
| ------------------ | ----------- | ---------------------------------------------------------------------- |
| `clice.executable` | _(bundled)_ | Path to a clice binary to use instead of the bundled one.              |
| `clice.mode`       | `pipe`      | Server transport; `socket` connects to an external server (debugging). |
| `clice.host`       | `127.0.0.1` | Host for socket mode.                                                  |
| `clice.port`       | `50051`     | Port for socket mode.                                                  |

## Troubleshooting

Server logs live in the `clice` output channel and in your workspace's
`.clice/logs/`. If the server crashes, please attach the newest log there to a
[GitHub issue](https://github.com/clice-io/clice/issues) — releases ship
symbol packages that let us reconstruct the exact stack.

Extension development is documented in the
[contributor guide](https://docs.clice.io/clice/dev/extension).
