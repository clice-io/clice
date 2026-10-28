---
issues: [clangd#2667]
---

# Collapsed text

A folded range shows a placeholder, such as `{...}` for a body, while the line it starts on stays visible

A client folding by characters, such as Zed, shows the placeholder in place
of the range. A client folding whole lines, such as Neovim, shows its text in
place of the whole first line, so it gets the first line followed by `...`
instead (see "Closing lines fold in").

> **Client support**: VS Code does not show `collapsedText` yet
> ([vscode#70794](https://github.com/microsoft/vscode/issues/70794)). A client
> that ignores it still folds, without the placeholder.
