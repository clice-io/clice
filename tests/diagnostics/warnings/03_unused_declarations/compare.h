#pragma once

template <typename T>
bool same(const T& a, const T& b) {
    return a == b;
}

template <typename T>
bool equal(const T& a, const T& b) {
    return a.equals(b);
}
