// A function with a deduced return type must be defined before its first
// use: moving its definition after a definition that calls it would break
// the file, so nothing is offered.

struct §(cls)S {
    int b();
    auto a();
};

auto S::a() {
    return 1;
}

int S::b() {
    return a();
}
