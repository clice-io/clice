#pragma once
#include "core/convert.h"

template <class T>
int converted(const T& value) {
    return ops::convert(value);
}
