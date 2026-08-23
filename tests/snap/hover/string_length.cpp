/// # Expression Context
///
/// ## String literals — the length reported on hover
///
/// - status: partial
/// - issues: clangd#1016
/// - order: 5
///
/// A string-literal card reports the array type and its size in bytes
/// (`const char[6]`, `Size: 6 bytes` — the length plus the null
/// terminator), not an explicit character count.

namespace string_length {

const char *greeting = §(01_string)"hello";

}
