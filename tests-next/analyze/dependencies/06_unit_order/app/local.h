// Declares lib's widen itself, as toml++ declares the Windows functions it
// calls.
#pragma once

int widen(int x);

inline int twice(int x) {
    return widen(x) * 2;
}
