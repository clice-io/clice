#pragma once
#include "core/str.h"

struct Clock {
    Str name;

    int size() const {
        return length(name);
    }
};
