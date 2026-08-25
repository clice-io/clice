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
/// (the PCH) as well as ordinary ones later in the file.

#include §(preamble_include)"panel.h"

int build() {
    return dimension();
}

#include §(late_include)"extra.h"

int total() {
    return build() + spacing();
}
