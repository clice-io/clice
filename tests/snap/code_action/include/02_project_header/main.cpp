/// # Include for a project symbol
///
/// - status: supported
/// - verify: both
/// - snap: separate
/// - diagnostics: expected
/// - indexing: true
///
/// A name declared in a project header the file does not include offers that header, spelled relative to the file
///
/// The candidates come from the project index: `lib.h` is known because
/// another source file includes it.

// snap: The inspect path has no index, so no header is found and nothing
// snap: is offered; the server path pins the directive the index resolved.

// indexed: helper

int main() {
    return §(helper)helper(1);
}
