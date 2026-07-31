/// # Type Hints
///
/// ## Type sugar and the length limit — aliases keep their spelling; over-long types fall back to the sugared name
///
/// - status: supported
/// - issues: clangd#1298, clangd#1357
/// - order: 2

using Integer = int;

Integer make_alias();

template <typename A, typename B, typename C>
struct extremely_long_template_name {};

using Compact = extremely_long_template_name<int, char, bool>;

Compact make_compact();

extremely_long_template_name<Integer, Integer, Integer> make_long();

template <typename T, typename U = int>
struct Defaulted {};

Defaulted<float> make_defaulted();

void use() {
    auto aliased = make_alias();
    auto shortened = make_compact();
    // No sugar short enough to fall back to: the hint is dropped.
    auto dropped = make_long();
    // Default template arguments never print.
    auto defaulted = make_defaulted();
}
