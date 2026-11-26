constexpr int column = __builtin_COLUMN();

#ifndef FROM_HOST
#define FROM_HOST 0
#endif

static_assert(column == 24 || FROM_HOST);
