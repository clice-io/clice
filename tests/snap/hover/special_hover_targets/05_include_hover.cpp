/// # Include directive hover
///
/// - status: supported
/// - flags: ["-I${corpus}"]
///
/// Hovering an `#include` shows the resolved header path
///
/// The card resolves the quoted header to its file on disk.

#include "own_h§(include_path)eader.h"

int use = own_header_value;
