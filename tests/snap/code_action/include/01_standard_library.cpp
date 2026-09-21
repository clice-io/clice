/// # Standard library include
///
/// - status: supported
/// - issues: clangd#1017
/// - diagnostics: expected
///
/// An unresolved standard library name offers the header declaring it, from the standard library mapping
///
/// The directive goes after the file's last include. An unqualified name
/// also tries the `std` namespace.

#include "support.h"

void f() {
    std::§(vector)vector<int> values;
    §(printf)printf("%d", 1);
}
