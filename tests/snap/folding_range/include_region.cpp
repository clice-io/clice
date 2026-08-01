/// # Fold Kinds
///
/// ## Include region folding — consecutive `#include` directives
///
/// - status: unsupported
/// - order: 9

#include <vector>       // ┐
#include <string>       // │ foldable region
#include <algorithm>    // ┘

#include "app.h"        // ┐ separate region
#include "config.h"     // ┘ (blank line separates)
