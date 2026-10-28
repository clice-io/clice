#pragma once

namespace pch {

struct Counter {
    int count = 0;

    int next() {
        return ++count;
    }
};

}  // namespace pch
