#include "core/mode.h"
#include "core/width.h"

int str_mode() {
    ModeB mode;
    return sizeof(mode) + width();
}
