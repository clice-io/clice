module;
#include "version.h"
export module app;
import lib;

void first(server& s) {
    s.stop();
}

sink use() {
    return sink(app::version);
}
