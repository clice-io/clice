#pragma once

// A marker §(here) opts this support header into the snapshot:
// the include_next links live in this file, not in the fixture entry.
#define WRAP_FIRST 1

#if __has_include_next(<wrap.h>)
#include_next <wrap.h>
#endif
