/// # Go to Definition
///
/// ## Go-to-definition on `#include` directives
///
/// - status: supported
/// - verify: server
/// - order: 4
///
/// Invoked on an `#include` line, go-to-definition opens the included
/// file. This works for the leading includes compiled into the preamble
/// (the PCH) as well as ordinary ones.

#include §(include_line)"panel.h"

int build() {
    return dimension();
}
