/// # Macro-expanded paths
///
/// - status: supported
/// - issues: clangd#2375
///
/// `#include MACRO` links the directive argument to the expanded target

#define HEADER "header_b.h"
#include HEADER
