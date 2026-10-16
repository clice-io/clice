#include "fmt.h"

void report(int count, double ratio, const char* name) {
    fmt::print("{} items, {:>8.2f} ratio, {{braces}}\n", count, ratio);
    log_line("%s: %d of %-8.3f%%\n", name, count, ratio);
    const char* plain = "{} stays text";
}
