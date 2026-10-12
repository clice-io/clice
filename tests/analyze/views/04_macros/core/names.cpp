// ENTRY reaches only the fragment its defining source pastes in.
enum Names {
#define ENTRY(x) x,
#include "core/table.inc"
#undef ENTRY
};

int first() {
    return alpha;
}
