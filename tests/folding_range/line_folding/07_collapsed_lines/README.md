# Closing lines fold in

A client that shows collapsed text folds a block's closing line too, `int area(Point p) {...}`

Each fold shows its first line followed by `...`; when the closing brace,
`#endif` or `*/` sits on a line of its own, that line folds in and shows
after the `...`. A closing line that opens the next block, such as
`} else {`, stays visible.

> **Client support**: Neovim shows the text through `vim.lsp.foldtext()`.
> VS Code shows no collapsed text, so the closing line stays visible there
> ([vscode#3352](https://github.com/microsoft/vscode/issues/3352)).
