/// # Find References
///
/// ## Cross-TU find references
///
/// - status: supported
/// - verify: server
/// - order: 1
///
/// Find references gathers uses from other files too: a function
/// defined in one source and called from a sibling reports both call
/// sites together with the declaration in the shared header, not only the
/// uses in the current file.

#include "shared.h"

int run(int value) {
    return §(use)compute(value);
}
