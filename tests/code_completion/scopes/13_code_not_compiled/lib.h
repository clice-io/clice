#pragma once

#define CELL(n) void c##n();
#define CELLS_10(n)                                                                                \
    CELL(n##0) CELL(n##1) CELL(n##2) CELL(n##3) CELL(n##4)                                         \
    CELL(n##5) CELL(n##6) CELL(n##7) CELL(n##8) CELL(n##9)
#define CELLS_100(n)                                                                               \
    CELLS_10(n##0) CELLS_10(n##1) CELLS_10(n##2) CELLS_10(n##3) CELLS_10(n##4)                     \
    CELLS_10(n##5) CELLS_10(n##6) CELLS_10(n##7) CELLS_10(n##8) CELLS_10(n##9)

// More names than a completion takes from the index.
namespace lib {
CELLS_100(1)
CELLS_100(2)
}  // namespace lib
