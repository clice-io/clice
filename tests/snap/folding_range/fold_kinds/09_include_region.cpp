/// # Include region folding
///
/// - status: unsupported
///
/// Consecutive `#include` directives

#include <vector>       // ┐
#include <string>       // │ foldable region
#include <algorithm>    // ┘

#include "app.h"        // ┐ separate region
#include "config.h"     // ┘ (blank line separates)
