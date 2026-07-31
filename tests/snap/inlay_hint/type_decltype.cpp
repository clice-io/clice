/// # Type Hints
///
/// ## `decltype` spellings — the underlying type shows next to the written `decltype`
///
/// - status: supported
/// - order: 6

int source();

decltype(source()) value = 1;

int& ref = value;
// decltype(auto) preserves the reference.
decltype(auto) forwarded = ref;

// Every written decltype spelling hints: declarators, alias targets,
// return types and functional casts.
const decltype(0)& bound = value;

decltype(0) declared();

auto trailing() -> decltype(0);

template <class, class>
struct Wrap;

using Alias = Wrap<decltype(0), float>;

auto constructed = decltype(0){};
