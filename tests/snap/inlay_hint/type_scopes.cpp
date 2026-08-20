/// # Type Hints
///
/// ## Scope suppression — namespace qualifiers drop from hints; class scopes stay
///
/// - status: supported
/// - order: 10

namespace outer {
namespace inner {

struct S1 {};
S1 make_s1();
auto x = make_s1();

struct S2 {
    template <typename T>
    struct Nested {};
};

S2::Nested<int> make_nested();
auto y = make_nested();

}  // namespace inner
}  // namespace outer
