/// # Single-line constructs stay unfolded — a fold that hides nothing is noise
///
/// - status: supported

namespace tiny { }

struct Empty {};

enum Flags { A, B };

void noop() {}

int values[] = {1, 2, 3};

auto lambda = [](int x) { return x; };

int result = lambda(42);
