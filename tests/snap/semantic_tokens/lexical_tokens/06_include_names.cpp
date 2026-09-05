/// # Header names
///
/// - status: supported
/// - flags: ["-I${corpus}"]
///
/// Quoted and angled `#include` filenames, including the split `# include` form

#include "inc/angled.h"
#include <angled.h>
# include "inc/angled.h"

int after_includes = 0;
