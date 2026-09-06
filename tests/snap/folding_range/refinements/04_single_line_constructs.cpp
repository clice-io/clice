/// # Single-line constructs stay unfolded
///
/// - status: supported
///
/// A fold that hides nothing is noise

namespace tiny { }

struct Empty {};

enum Flags { A, B };

void noop() {}

int values[] = {1, 2, 3};

auto lambda = [](int x) { return x; };

int result = lambda(42);
