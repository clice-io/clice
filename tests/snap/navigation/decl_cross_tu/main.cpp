/// # Go to Declaration
///
/// ## Cross-TU go-to-declaration
///
/// - status: supported
/// - verify: server
/// - order: 1
///
/// Go-to-declaration on a use resolves across the whole project: the
/// prototype lives in a shared header and the out-of-line definition in a
/// sibling source, and both are offered from a use in another file.

#include "shared.h"

int run(int value) {
    return §(use)scale(value);
}
