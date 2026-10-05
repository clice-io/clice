# clice-zed

This is the [Zed](https://zed.dev) extension for [clice](https://github.com/clice-io/clice).

## Server binary

The extension starts the first of:

1. `lsp.clice.binary.path` from your settings. Zed launches it directly with `lsp.clice.binary.arguments`, so the arguments must include `serve`:

   ```json
   {
     "lsp": {
       "clice": {
         "binary": { "path": "/path/to/clice", "arguments": ["serve"] }
       }
     }
   }
   ```

2. `clice` on your `PATH`.
3. The newest clice release from GitHub, downloaded into the extension's work directory and replaced when a newer release appears. When the release lookup or the download fails, the last downloaded version keeps serving.

`lsp.clice.settings.release_channel` selects the releases to download: `"pre-release"` (the default, nightly builds) or `"stable"`.

## Using clice instead of clangd

Zed runs its built-in clangd for C and C++ as well. To use clice alone:

```json
{
  "languages": {
    "C++": { "language_servers": ["clice", "!clangd", "..."] },
    "C": { "language_servers": ["clice", "!clangd", "..."] }
  }
}
```
