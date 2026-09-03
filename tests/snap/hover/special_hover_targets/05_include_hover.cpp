/// # Include directive hover — hovering an `#include` shows the resolved header path
///
/// - status: supported
/// - flags: ["-I${corpus}"]
///
/// The card resolves the quoted header to its file on disk.

#include "own_h§(include_path)eader.h"

int use = own_header_value;
