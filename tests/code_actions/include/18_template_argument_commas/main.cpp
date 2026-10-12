// std::move is declared in <utility> with one parameter and in <algorithm>
// with three. The commas of a template argument list separate no arguments,
// and the text alone cannot tell one from a less-than: neither header is
// offered for this call.

#include <tuple>

void f() {
    std::§(move)move(std::tuple<int, int, int>{});
}
