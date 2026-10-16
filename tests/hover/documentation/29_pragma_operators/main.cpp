#define COLD_BEGIN _Pragma("clang attribute push(__attribute__((cold)), apply_to = function)")
#define COLD_END _Pragma("clang attribute pop")

COLD_BEGIN

/// Rarely runs.
void §(rarely)rarely();

COLD_END
