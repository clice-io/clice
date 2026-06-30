// auto initialized from explicitly spelled types
namespace auto_explicit_type {

struct Box {
    explicit Box(int);
};

void test(int val) {
    auto x = static_cast<int>(val);
    auto y = int{42};
    auto object = Box{42};
    auto z = val + 1;
}

}  // namespace auto_explicit_type
