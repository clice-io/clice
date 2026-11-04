#pragma once

#define CELL(n) void c##n();
#define CELLS_10(n)                                                                                \
    CELL(n##0) CELL(n##1) CELL(n##2) CELL(n##3) CELL(n##4)                                         \
    CELL(n##5) CELL(n##6) CELL(n##7) CELL(n##8) CELL(n##9)
#define CELLS_100(n)                                                                               \
    CELLS_10(n##0) CELLS_10(n##1) CELLS_10(n##2) CELLS_10(n##3) CELLS_10(n##4)                     \
    CELLS_10(n##5) CELLS_10(n##6) CELLS_10(n##7) CELLS_10(n##8) CELLS_10(n##9)

#define CRAM(n) void cram(Tag<n>);
#define CRAMS_10(n)                                                                                \
    CRAM(n##0) CRAM(n##1) CRAM(n##2) CRAM(n##3) CRAM(n##4)                                         \
    CRAM(n##5) CRAM(n##6) CRAM(n##7) CRAM(n##8) CRAM(n##9)

namespace lib {

// Below the namespace, more names starting with `c` than a completion takes
// from the index, each matching `c` better than the namespace's own.
namespace detail {
CELLS_100(1)
CELLS_100(2)
CELLS_100(3)
CELLS_100(4)
CELLS_100(5)
CELLS_100(6)
CELLS_100(7)
CELLS_100(8)
CELLS_100(9)
}  // namespace detail

template <int N>
struct Tag {};

// More overloads of one name than a completion takes names from the index.
CRAMS_10(1)
CRAMS_10(2)
CRAMS_10(3)
CRAMS_10(4)
CRAMS_10(5)
CRAMS_10(6)
CRAMS_10(7)
CRAMS_10(8)
CRAMS_10(9)
CRAMS_10(10)
CRAMS_10(11)

void make_cell();

enum Shade { shade_cell };

}  // namespace lib
