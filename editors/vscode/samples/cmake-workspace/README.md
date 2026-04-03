# VS Code CMake Sample

This workspace is a standalone CMake project for attaching the VS Code extension to a real `clice` session.

`clice` already auto-detects `build/compile_commands.json`, so this sample does not need any helper scripts or extra CMake glue.

The workspace contains two entry points:

- `main.cc`: a traditional include-based example.
- `main_module.cc`: a C++20 modules example that imports `greeting_module.cppm`.

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

## C++20 Modules

The module example is enabled automatically when CMake is using a compiler it can scan for C++20 modules:

- Clang 16+
- GCC 14+
- MSVC 19.34+

If the active compiler is older than that, CMake still configures the workspace and builds `sample_app`, but it skips `sample_module_app`.

When you do have a supported compiler, `main_module.cc` and `greeting_module.cppm` will also appear in `build/compile_commands.json`, which makes this workspace useful for testing clice's module handling in an editor.
