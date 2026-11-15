module;

export module compat.pch;

import std;

export int pch_unit() {
    std::vector<int> values{1, 2};
    return static_cast<int>(values.size());
}
