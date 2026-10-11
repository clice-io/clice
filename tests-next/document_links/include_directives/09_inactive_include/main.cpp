// An include in a branch the preprocessor skips is not followed, so it has
// no link, even where the header exists.
#ifdef _WIN32
#include "windows_api.h"
#else
#include "posix_api.h"
#endif
