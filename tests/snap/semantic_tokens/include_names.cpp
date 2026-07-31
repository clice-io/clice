/// # Lexical Tokens
///
/// ## Header names — quoted and angled `#include` filenames, including the split `# include` form
///
/// - status: supported
/// - order: 5

#include "inc/angled.h"
#include <angled.h>
# include "inc/angled.h"

int after_includes = 0;
