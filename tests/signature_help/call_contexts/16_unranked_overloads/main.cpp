#include "rules.h"

struct Pick {
    int operator()() const;
    float operator()();
};

void use() {
    With<void>(§(implicit));
    Pick pick;
    pick(§(call_operator));
}
