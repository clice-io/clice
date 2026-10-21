#pragma once

static int offset() {
    return 0;
}

inline int helper() {
    return 1 + offset();
}
