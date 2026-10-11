#pragma once

struct Str {
    int n;
};

int length(const Str& s);

static int helper() {
    return 1;
}

inline int uses_helper() {
    return helper();
}
