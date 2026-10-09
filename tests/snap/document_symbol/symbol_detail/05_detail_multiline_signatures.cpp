/// # Multiline signature ranges
///
/// - status: supported
/// - issues: clangd#2221
///
/// The symbol range starts at the beginning of the declaration and spans the
/// full signature, so editor sticky scroll anchors correctly

struct Config {};

void process_data(
    const Config& cfg,
    int flags
) {}
