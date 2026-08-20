// The space in the include path must survive the round trip as %20 in the
// server URI; a target that skips percent-encoding fails normalization.
#include "sub dir/escaped header.h"

int use_escaped = escaped_value;
