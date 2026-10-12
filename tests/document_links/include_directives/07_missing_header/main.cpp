// A header that does not exist has nowhere to link to; the includes around
// it still link.
#include "present.h"
#include "absent.h"

int use = present_value;
