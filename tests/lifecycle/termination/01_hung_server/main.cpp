constexpr long fib(long n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
constexpr long x = fib(90);
