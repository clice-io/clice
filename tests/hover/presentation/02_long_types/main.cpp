// A long declaration wraps in the definition block as clang-format lays it
// out; the type lines above it stay on one line each.

namespace clang_format_types {

template <typename A, typename B, typename C, typename D>
struct Tuple {};

using Row = Tuple<int, long, unsigned, char>;

Tuple<Row, Row, Row, Row> §(nested)table;

void §(parameters)consume(Tuple<Row, Row, Row, Row> first, Tuple<Row, Row, Row, Row> second, Tuple<Row, Row, Row, Row> third);

}
