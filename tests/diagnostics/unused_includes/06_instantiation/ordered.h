#pragma once
template <class T>
bool ordered(T a, T b) {
    return !(b < a);
}
