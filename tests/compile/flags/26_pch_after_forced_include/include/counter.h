#pragma once

#ifndef GLOBAL_STEP
#error "the build forces global.h in first"
#endif

namespace pch {

struct Counter {
    int count = 0;

    int next() {
        return count += §(step)GLOBAL_STEP;
    }
};

}  // namespace pch
