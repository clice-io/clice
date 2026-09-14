// - verify: inspect

// One unguarded file included twice under different macro contexts is
// two inclusions with their own units and digests; a dependency names
// the inclusion it points at.
#define NAME first
#include "twice.inc"
#undef NAME
#define NAME second
#include "twice.inc"

int both() { return first + second; }
int one; int other;
int use_one() { return other; }
