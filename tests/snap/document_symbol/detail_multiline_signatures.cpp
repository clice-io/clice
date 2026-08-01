/// # Symbol Detail
///
/// ## Multiline signature ranges — the symbol range starts at the beginning of the declaration and spans the full signature, so editor sticky scroll anchors correctly
///
/// - status: supported
/// - issues: clangd#2221
/// - order: 5

struct Config {};

void process_data(
    const Config& cfg,
    int flags
) {}
