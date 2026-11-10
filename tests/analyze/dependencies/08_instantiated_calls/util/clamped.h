#pragma once

template <class T>
int clamped(const T& value) {
    return ops::clamp(value);
}
