/// # Fold Kinds
///
/// ## Custom region folding (`#pragma region` / `#pragma endregion`)
///
/// - status: supported
/// - issues: clangd#1623
/// - order: 6
/// - snap: skip

// snap: skip because this file is directives-only, so the server's
// preamble PCH swallows the whole file and the server reply loses the region
// fold that the inspect-path (no-PCH) compile reports. Un-skip once the two
// paths agree.

#pragma region Configuration

int retry_count = 3;
int timeout_ms = 5000;

#pragma endregion
