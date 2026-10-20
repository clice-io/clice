#include <stdbool.h>

struct Flags {
    bool on;
};

static inline bool is_on(struct Flags flags) {
    return flags.on;
}
