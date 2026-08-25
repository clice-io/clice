/// # Include Directives
///
/// ## Quoted includes — `#include "..."` links to the resolved header file
///
/// - status: supported
/// - order: 1
///
/// Every include in the file is linked, not just the preamble run at
/// the top.

#include "header_a.h"
#include "header_b.h"
int x = 1;
#include "header_c.h"
