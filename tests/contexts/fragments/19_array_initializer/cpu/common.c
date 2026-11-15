struct cpu_id {
    int family;
    int model;
};

// Used past the table alone: compiled up to it, this code warns that
// `later` is unused, an error under -Werror.
static int later(void) {
    return 1;
}

static struct cpu_id latest[] = {
#include "microcode/defs.h"
    {}
};

int count(void) {
    return later() + sizeof(latest) / sizeof(latest[0]);
}
