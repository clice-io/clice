// - verify: server
// - indexing: true
//
// Aliases at namespace and class scope are found like what they name; the
// header stays closed, so every hit comes from the background index. An
// alias inside a function stays local.

// indexed: Widget

// query: Size
// query: Handle
// query: Ptr
// query: id_type
// query: di
// query: Hidden

#include "types.h"

int anchor = 0;
