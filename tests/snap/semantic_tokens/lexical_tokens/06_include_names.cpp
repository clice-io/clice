/// # Header names — quoted and angled `#include` filenames, including the split `# include` form
///
/// - status: supported
/// - flags: ["-I${corpus}"]

#include "inc/angled.h"
#include <angled.h>
# include "inc/angled.h"

int after_includes = 0;
