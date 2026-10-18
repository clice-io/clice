#pragma once

struct Beta {
    Alpha alpha;
};

inline int beta_value() {
    return alpha_value() + 1;
}
