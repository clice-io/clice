#include "fmt.h"

void report(Logger logger, int count, double ratio, const char* name) {
    fmt::print("{} items, {:>8.2f} ratio, {{braces}}\n", count, ratio);
    log_line("%s: %d of %-8.3f%%\n", name, count, ratio);
    log_line(R"(%d raw \n)", count);
    log_line("%2$s: %1$*3$d\n", count, name, 8);
    logger.log("%d member", count);
    logger("%d call", count);
    const char* plain = "{} stays text";
}

void scan(char** owned, char* word, int* count) {
    read_line("%ms %15[^,\n], %*d %d", owned, word, count);
}
