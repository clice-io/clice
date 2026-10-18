namespace std {
template <class Char>
struct basic_string {};

typedef basic_string<char> string;

namespace chrono {
struct seconds {};
}  // namespace chrono
}  // namespace std

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

inner::S1 make_inner();
auto z = make_inner();

}  // namespace outer

namespace lib {
struct Thing {};

template <class A, class B>
struct Pair {};

Thing make();
Pair<std::string, Thing> pair();
}  // namespace lib

namespace {
struct Hidden {};
Hidden hide();
}  // namespace

std::string name();
std::chrono::seconds wait();

void use() {
    auto inner = outer::make_inner();
    auto thing = lib::make();
    auto both = lib::pair();
    auto text = name();
    auto waited = wait();
    auto hidden = hide();
}

namespace app {
using namespace lib;
auto thing = make();
}  // namespace app
