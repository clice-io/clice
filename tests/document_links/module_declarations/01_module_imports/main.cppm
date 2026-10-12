export module app;

import lib;
import :part;
export import lib.extra;

export int run() {
    return lib_value() + part_value() + extra_value();
}
