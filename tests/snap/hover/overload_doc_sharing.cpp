/// # Documentation
///
/// ## Overload doc sharing — a later overload with no comment reuses the first overload's documentation
///
/// - status: partial
/// - order: 5
/// - issues: clangd#2506
///
/// Consecutive overloads often document only the first; a later undocumented
/// overload should reuse that shared description. clice does not share it
/// yet — the later overload's card carries no description.

namespace overloads {
/// Opens a file.
void §(01_first)open(const char* path);
void §(02_second)open(const char* path, int flags);
}
