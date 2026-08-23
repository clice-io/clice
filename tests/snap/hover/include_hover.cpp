/// # Special Hover Targets
///
/// ## Include directive hover — hovering an `#include` shows the resolved header path
///
/// - status: supported
/// - order: 5
/// - snap: skip
///
/// The card resolves the quoted header to its file on disk.

// snap: skip — the card renders the header's absolute path, which the hover
// snapshot renderer does not rewrite to ${WS}; the inspect path (real corpus
// dir) and the server path (throwaway workspace) therefore produce different,
// machine-specific paths that cannot be pinned as a shared snapshot.

#include "own_h§(include_path)eader.h"

int use = own_header_value;
