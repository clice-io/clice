#pragma once

static int helper() {
    return 1;
}

namespace {
inline int hidden() {
    return 2;
}
}  // namespace

inline int uses_helper() {
    return helper();
}

static const int limit = 9;
