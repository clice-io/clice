#include "rules.h"

struct Pick {
    int operator()() const;
    float operator()();
};

struct Choose {
    template <class T = int>
    float operator()();
    int operator()() const;
};

void use() {
    With<void>(§(implicit));
    Pick pick;
    pick(§(call_operator));
    Choose choose;
    choose(§(template_call_operator));
}
