// snap: the §(here) marker opts this support header into the per-file
// snapshot — the include_next links live here, not in the fixture entry.
#pragma once

#define WRAP_FIRST 1

#if __has_include_next(<wrap.h>)
#include_next <wrap.h>
#endif
