// An alias of an alias template that selects through a partial
// specialization resolves to the type finally chosen.

namespace alias_selection {
template <bool Select, typename Left, typename Right> struct choose {
    using type = Left;
};
template <typename Left, typename Right> struct choose<false, Left, Right> {
    using type = Right;
};
template <bool Select, typename Left, typename Right>
using selected = typename choose<Select, Left, Right>::type;

using integ§(typedef_chain)er = selected<true, long, double>;
}
