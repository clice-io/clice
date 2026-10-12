// Attribute documentation in the markup forms the first cases do not
// reach: *emphasis*, auto-numbered lists, a backslash escape after a
// literal, and a note directive spelled with a capital.

namespace markup_forms {

struct Logger {
    Logger();
};

Logger logger __attribute__((init_§(emphasis)priority(200)));

int log(const char* format, ...) __attribute__((for§(numbered_list)mat(printf, 1, 2)));

const int& pick(const int& value [[clang::lifetime§(escape)bound]]);

void* allocate(unsigned long size) __attribute__((alloc_§(capitalized_note)size(1)));

}
