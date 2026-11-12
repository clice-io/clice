#pragma clang diagnostic ignored "-Wliteral-conversion"
#pragma clang diagnostic ignored "-Wnull-conversion"

void literals() {
    while ("foo") {
    }

    while ("foo but this time it is very long") {
    }

    while (true) {
    }

    while (1) {
    }

    while (1.5) {
    }

    while ("a\nb\ncdefghij") {
    }

    while (nullptr) {
    }
}

namespace ns {
int Var;
int func();
int check(int);
int ready(int mode = 0);
struct S {
    int Field;
    int method() const;
};
}

void references() {
    while (ns::Var) {
    }

    while (ns::func()) {
    }

    while (ns::S{}.Field) {
    }

    while (ns::S{}.method()) {
    }

    while (ns::check(1)) {
    }

    while (ns::ready()) {
    }
}

struct Number {
    Number(int);
    Number(int, int);
    explicit operator bool();
};

void conversions(int I) {
    while (float(I)) {
    }

    while (Number(I)) {
    }

    while (Number(I, I)) {
    }
}

using Integer = int;
void operators(Integer I) {
    while (++I) {
    }

    while (I++) {
    }

    while (+(I + I)) {
    }

    while (I < 0) {
    }

    while ((I + I) < I) {
    }

    while (I < (I + I)) {
    }

    while ((I + I) < (I + I)) {
    }
}
