/// # Default library — symbols declared in system headers
///
/// - status: supported

int before_includes = 0;

#include <syslib.h>

int used = §system_helper();
