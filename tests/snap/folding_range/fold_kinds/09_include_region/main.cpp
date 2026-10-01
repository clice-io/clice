/// # Include region folding
///
/// - status: supported
///
/// Consecutive include directives fold as one run below the first include
///
/// A blank line, a comment line or another directive ends the run; includes
/// inside a conditional branch fold within it, whether or not the branch is
/// taken.

#include "alpha.h"       // ┐
#include "beta.h"        // │ one run
#include "gamma.h"       // ┘

#include "delta.h"       // a lone include stays unfolded

#include "alpha.h"
// a comment line ends the run
#include "beta.h"
#define AFTER_BETA
#include "gamma.h"

#ifdef _WIN32
#include "windows_api.h" // ┐ a run in a skipped branch
#include "windows_io.h"  // ┘
#else
#include "posix_api.h"   // ┐ a run in the taken branch
#include "posix_io.h"    // ┘
#endif
