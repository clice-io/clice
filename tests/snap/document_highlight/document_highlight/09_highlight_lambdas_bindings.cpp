/// # Lambda captures and structured bindings
///
/// - status: supported
///
/// A variable a lambda captures highlights in the capture list and the lambda
/// body; an init capture is a variable of its own
///
/// Each name a structured binding introduces is a symbol of its own.

struct Pair {
    int first;
    int second;
};

int combine(Pair pair) {
    int §(base)base = 1;
    auto add = [&base](int step) { base += step; return base; };
    auto copy = [§(init_capture)renamed = base] { return renamed + 1; };
    auto [§(binding)left, right] = pair;
    left = right;
    return add(left) + copy() + base;
}
