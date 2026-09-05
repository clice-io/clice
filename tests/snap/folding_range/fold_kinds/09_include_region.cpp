/// # Include region folding
///
/// - status: unsupported
///
/// Consecutive include directives do not form folding ranges yet

#include <vector>       // ┐
#include <string>       // │ foldable region
#include <algorithm>    // ┘

#include "app.h"        // ┐ separate region
#include "config.h"     // ┘ (blank line separates)
