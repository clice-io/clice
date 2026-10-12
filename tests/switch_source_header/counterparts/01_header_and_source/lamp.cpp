#include "lamp.h"

static int level = 0;

int lamp_brightness() {
    return level;
}

void lamp_toggle() {
    level = level == 0 ? 100 : 0;
}
