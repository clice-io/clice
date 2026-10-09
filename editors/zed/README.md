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
3. A clice release from GitHub, downloaded into the extension's work directory. The extension checks for a newer release once per Zed session, when clice first starts; when the check or the download fails, the last downloaded version keeps serving.

`lsp.clice.settings.release_channel` selects the releases to download: `"stable"` (the default) takes the newest stable release, or the newest pre-release while no stable release ships your platform; `"pre-release"` always takes the newest nightly build:

```json
{ "lsp": { "clice": { "settings": { "release_channel": "pre-release" } } } }
```

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

Zed's `editor: switch source header` asks clangd alone, so it does nothing once clangd is off, and an extension cannot add the command for clice.
