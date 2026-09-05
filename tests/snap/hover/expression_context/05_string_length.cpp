/// # String literals
///
/// - status: partial
/// - issues: clangd#1016
///
/// The length reported on hover
///
/// A string-literal card reports the array type and its size in bytes
/// (`const char[6]`, `Size: 6 bytes` — the length plus the null
/// terminator), not an explicit character count.

namespace string_length {

const char *greeting = §(01_string)"hello";

}
