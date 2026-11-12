// stdbool.h's `bool` reaches main.c only through the preamble, and main.c
// never spells it: the macro stays unread until the hover asks.
#include "flags.h"

int use(struct Flags flags) {
    return §(function)is_on(flags) + flags.§(member)on;
}
