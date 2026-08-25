/// # Go to Definition
///
/// ## Index-based cross-TU go-to-definition
///
/// - status: supported
/// - verify: server
/// - order: 1
///
/// A use in one translation unit resolves to a definition supplied by a
/// sibling source, drawing on the project-wide index rather than the
/// current file alone.

#include "shared.h"

int run(int value) {
    return §(cross_use)transform(value);
}
