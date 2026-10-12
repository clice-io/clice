namespace tiny { }

struct Empty {};

enum Flags { A, B };

void noop() {}

int values[] = {1, 2, 3};

auto lambda = [](int x) { return x; };

int result = lambda(42);
