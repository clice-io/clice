void branches(bool cond) {
    if (cond)
        ;

    if (cond) {
    }

    if (cond) {
    } else {
    }

    if (cond) {
    } else if (!cond) {
    }

    if (cond) {
    } else {
        if (!cond) {
        }
    }

    if (auto X = cond) {
    }

    if (int i = 0; i > 10) {
    }
}

void compile_time() {
    if consteval {
        int x = 1;
    }
}

void loops() {
    while (true)
        ;

    while (true) {
    }

    do {
    } while (true);

    for (;true;) {
    }

    for (int I = 0; I < 10; ++I) {
    }

    int Vs[] = {1, 2, 3};
    for (auto V : Vs) {
    }
}

void choose(int I) {
    switch (I) {
        case 0: break;
    }
}

class A {};
using Predicate = bool (A::*)();
void member_call(A* a, Predicate p) {
    if ((a->*p)()) {
    }
}
