/// # Fold from the declaration line for function/class bodies — keep the signature visible when folded
///
/// - status: unsupported
/// - issues: clangd#2666
/// - order: 2
///
/// > **Client support**: this depends on the client interpreting
/// > `FoldingRange.startLine` correctly. VS Code uses the line _after_
/// > `startLine` as the first hidden line, so setting `startLine` to the
/// > declaration line achieves the desired effect. However, VS Code still
/// > leaves the closing `}` on a separate line rather than collapsing it onto
/// > the signature line ([vscode#3352](https://github.com/microsoft/vscode/issues/3352)
/// > — still open). Other clients may differ.

struct Config {
    int width;
    int height;
};

// desired when folded: int process_data(const Config& cfg) {...}
// not:                 {... (signature hidden above fold)}
int process_data(const Config& cfg) {
    int area = cfg.width * cfg.height;
    return area;
}
