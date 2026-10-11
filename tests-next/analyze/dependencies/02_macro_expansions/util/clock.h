#pragma once

struct Clock {
    int size() const {
        return 1;
    }
};

#define UTIL_CLOCK() Clock{}
