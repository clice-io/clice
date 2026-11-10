#pragma once

template <class T>
int made(const T& value) {
    return T::make(value);
}
