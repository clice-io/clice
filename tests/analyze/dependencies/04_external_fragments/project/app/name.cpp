// The workspace is project/: a fragment outside it is a library's
// generated table. What the switch pastes names red and green, not blue;
// gamma, pasted into core/names.h, belongs to that header.
#include "core/color.h"
#include "core/names.h"

const char* name(Color color) {
    switch(color) {
#define KIND(x) case x: return #x;
#include "kinds.inc"
#undef KIND
    default: return "";
    }
}

int count() {
    return gamma;
}
