---
issues: [clangd#2666]
---

# Declaration-line folding

A declaration's body folds from the line holding its name

When the opening brace sits below the name, on a line of its own or after a
signature spanning several lines, a client that folds whole lines starts the
fold on the name's line, so the folded declaration still shows what it is. A
client folding by characters starts at the brace and keeps everything before
it visible anyway.

> **Client support**: VS Code still leaves the closing `}` on a line of its
> own rather than joining it to the signature line
> ([vscode#3352](https://github.com/microsoft/vscode/issues/3352)).
