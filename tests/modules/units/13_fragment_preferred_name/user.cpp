#include "version.h"
import lib;

void first(server& s) {
    s.stop();
}

sink use() {
    return sink(app::version);
}
