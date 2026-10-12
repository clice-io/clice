#pragma once

inline int scale(int factor, int offset) {
    return factor + offset;
}

§(body)⟦inline int twice(int value) {
    auto doubled = scale(value, value);
    return doubled;
}⟧
