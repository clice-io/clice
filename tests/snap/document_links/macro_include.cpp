/// # Include Directives
///
/// ## Macro-expanded paths — `#include MACRO` links the directive argument to the expanded target
///
/// - status: supported
/// - order: 3
/// - issues: clangd#2375

#define HEADER "header_b.h"
#include HEADER
