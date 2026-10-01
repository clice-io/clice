// - diagnostics: expected

// A leading `#ifndef`/`#define`/`#endif` block that does not enclose the
// whole file defaults a feature macro; it is no include guard, and the
// directive goes after the include below it rather than into the block.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "support.h"

using Strings = std::§(string)string;
