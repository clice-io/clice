# VS Code CMake Sample

This workspace is a standalone CMake project for attaching the VS Code extension to a real `clice` session.

`clice` already auto-detects `build/compile_commands.json`, so this sample does not need any helper scripts or extra CMake glue.

## Prepare The Workspace

From this directory:

```sh
cmake -S . -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

That is enough to generate `build/compile_commands.json`, which `clice` can discover automatically when this folder is opened as the workspace.

If you also want the sample binary:

```sh
cmake --build build
```
