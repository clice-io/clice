// - verify: server
// - indexing: true
//
// Template parameters and unnamed scopes are no one's search target,
// whether the background index files them (the header stays closed) or an
// open file's own table holds them (`M`).

// indexed: Flag

// query: Flag
// query: N
// query: TT
// query: anonymous
// query: M

#include "noise.h"

template <int M>
struct OpenFlag {};

int anchor = 0;
