// Header names with a space, a non-ASCII letter, '#' and '+' survive the URI
// round trip: a space travels as %20, '#' as %23 rather than a fragment, '+'
// stays literal, and UTF-8 percent-encodes per byte.
#include "sub dir/escaped header.h"
#include "café.h"
#include "hash#header.h"
#include "plus+header.h"

int use = escaped_value + unicode_value + hash_value + plus_value;
