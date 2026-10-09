// - verify: server
// - indexing: true
//
// Template parameters, unnamed scopes and a function's locals are no one's
// search target, whether the background index files them (the header stays
// closed) or an open file's own table holds them (`M`, `local_value`).

// indexed: Flag

// query: Flag
// query: N
// query: TT
// query: anonymous
// query: M
// query: local_value

#include "noise.h"

template <int M>
struct OpenFlag {};

int anchor() {
    int local_value = 0;
    return local_value;
}
