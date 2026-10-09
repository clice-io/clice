/// # Go-to-definition on `#include` directives
///
/// - status: supported
/// - verify: server
///
/// Go-to-definition on an include opens the referenced file
///
/// Leading includes and ordinary includes later in the file behave alike.

#include §(preamble_include)"panel.h"

int build() {
    return dimension();
}

#include §(late_include)"extra.h"

int total() {
    return build() + spacing();
}
