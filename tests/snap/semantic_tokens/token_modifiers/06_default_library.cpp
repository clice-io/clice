/// # Default library
///
/// - status: supported
///
/// Symbols from system headers carry the default-library modifier

int before_includes = 0;

#include <syslib.h>

int used = §system_helper();
