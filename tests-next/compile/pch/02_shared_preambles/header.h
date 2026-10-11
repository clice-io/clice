#pragma once

#ifdef MODE
struct Config {
    int mode;
};
#else
struct Config {
    int plain;
};
#endif
