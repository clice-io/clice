#pragma once
int announce();

struct Plugin {
    static inline int registered = announce();
};
