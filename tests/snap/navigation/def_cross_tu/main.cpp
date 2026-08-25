/// # Go to Definition
///
/// ## Cross-TU go-to-definition
///
/// - status: supported
/// - verify: server
/// - order: 1
///
/// A use in one translation unit resolves to the definition supplied by
/// a sibling source — the answer spans the project, not the current
/// file alone.

#include "shared.h"

int run(int value) {
    return §(cross_use)transform(value);
}
