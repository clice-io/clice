/// # Macro-expanded paths — `#include MACRO` links the directive argument to the expanded target
///
/// - status: supported
/// - issues: clangd#2375

#define HEADER "header_b.h"
#include HEADER
