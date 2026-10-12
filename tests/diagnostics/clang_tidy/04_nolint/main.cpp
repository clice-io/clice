double first(int a, int b) { return a / b; } // NOLINT

// NOLINTNEXTLINE(bugprone-integer-division)
double second(int a, int b) { return a / b; }

double third(int a, int b) { return a / b; }

int quiet() { int unused; return 0; } // NOLINT

int loud() { int unused; return 0; }
