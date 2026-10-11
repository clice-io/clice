---
issues: [clangd#2667]
---

# Collapsed text

A folded range shows a placeholder, such as `{...}` for a body, while the line it starts on stays visible

> **Client support**: VS Code does not show `collapsedText` yet
> ([vscode#70794](https://github.com/microsoft/vscode/issues/70794)); Neovim
> does. A client that ignores it still folds, without the placeholder.
