/// # Overload doc sharing
///
/// - status: partial
/// - issues: clangd#2506
///
/// A later overload does not reuse the first overload's documentation yet
///
/// Consecutive overloads often document only the first; a later undocumented
/// overload has no description on its hover card.

namespace overloads {
/// Opens a file.
void §(01_first)open(const char* path);
void §(02_second)open(const char* path, int flags);
}
