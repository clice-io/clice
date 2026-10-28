// A captured variable highlights in the capture list and the lambda body; an
// init capture and each structured binding are symbols of their own.

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
