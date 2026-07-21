/// # `collapsedText` placeholder (LSP 3.17) — show a summary when folded
///
/// - status: supported
/// - issues: clangd#2667
/// - order: 1
///
/// > **Client support**: VS Code does **not** support `collapsedText` yet
/// > ([vscode#70794](https://github.com/microsoft/vscode/issues/70794) — still
/// > open); Neovim with nvim-lsp supports it natively. Clients that do not
/// > implement this field will silently ignore it — the folding still works,
/// > only the placeholder text is missing.

struct Config {
    int width;
    int height;
};

// When folded, the body collapses to a `{...}` placeholder while the
// signature stays visible: int process_data(const Config& cfg) {...}
int process_data(const Config& cfg) {
    return cfg.width * cfg.height;
}
