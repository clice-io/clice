namespace std {
namespace __detail {
struct Iter {};
}  // namespace __detail

struct Sequence {
    using iterator = __detail::Iter;
    iterator begin();
};

template <class... Ts>
struct Wrapper {
    using first_type = __type_pack_element<0, Ts...>;
    first_type get();
};

template <class... Ts>
Wrapper<Ts...> wrap(Ts...);

template <class F>
struct function;

template <class R, class... Args>
struct function<R(Args...)> {
    typedef R result_type;
    R operator()(Args...);
};

struct Root {
    using iterator = __detail::Iter;
    iterator find();
};

template <class T>
struct Derived : Root {};
}  // namespace std

void use(std::Sequence& sequence,
         std::function<std::__detail::Iter()>& next,
         std::Derived<std::__detail::Iter>& derived) {
    auto wrapped = std::wrap(sequence.begin()).get();
    auto called = next();
    auto found = derived.find();
}
