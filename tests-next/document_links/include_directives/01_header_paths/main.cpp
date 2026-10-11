#include "gauge.h"
#include <panel/dial.h>

int show() {
    return gauge_level() + dial_angle();
}
