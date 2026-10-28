#pragma once

// What <stdbool.h> defines: the topic compiles with -nostdinc.
#define bool _Bool

struct Flags {
    bool on;
};
