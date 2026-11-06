namespace nttp_array_size {

template <typename T, unsigned long N>
struct S {
    using type = T[N];
};

template <typename X>
struct test {
    using §(nttp_array_size)input = typename S<X, 3>::type;
};

}  // namespace nttp_array_size

namespace multi_dim_array {

template <typename T, unsigned long N>
struct S {
    using type = T[N][3];
};

template <typename X>
struct test {
    using §(multi_dim_array)input = typename S<X, 4>::type;
};

}  // namespace multi_dim_array

namespace array_of_pointer {

template <typename T, unsigned long N>
struct S {
    using type = T*[N];
};

template <typename X>
struct test {
    using §(array_of_pointer)input = typename S<X, 2>::type;
};

}  // namespace array_of_pointer

namespace function_param_pack {

template <typename... Ts>
struct A {
    using type = void(Ts...);
};

template <typename X>
struct test {
    using §(function_param_pack)input = typename A<X, int>::type;
};

}  // namespace function_param_pack

namespace underlying_type_transform {

enum class E : short {};

template <typename T>
struct wrap {
    using held = E;
};

template <typename T>
struct A {
    using type = __underlying_type(typename wrap<T>::held);
};

template <typename X>
struct test {
    using §(underlying_type_transform)input = typename A<X>::type;
};

}  // namespace underlying_type_transform

namespace decayed_parameter {

template <typename T>
struct A {
    using type = void(T[]);
};

template <typename X>
struct test {
    using §(decayed_parameter)input = typename A<X>::type;
};

}  // namespace decayed_parameter

namespace noexcept_substitute {

template <bool B>
struct A {
    using type = void() noexcept(B);
};

template <typename X, bool P>
struct test {
    using §(noexcept_substitute)input = typename A<P>::type;
};

}  // namespace noexcept_substitute

namespace array_size_forward {

template <typename T, unsigned long N>
struct S {
    using type = T[N];
};

template <typename X, unsigned long M>
struct test {
    using §(array_size_forward)input = typename S<X, M>::type;
};

}  // namespace array_size_forward

namespace param_decay_adjust {

template <typename T, typename U>
struct A {
    using type = void(T);
};

template <typename X>
struct test {
    using §(param_decay_adjust)input = typename A<int[2], X>::type;
};

}  // namespace param_decay_adjust

namespace abominable_add_pointer {

template <typename X, typename F>
struct wrap {
    using type = F;
};

template <typename X>
struct test {
    // A function type with qualifiers of its own has no pointer.
    using §(abominable_add_pointer)input = __add_pointer(typename wrap<X, int() const>::type);
};

}  // namespace abominable_add_pointer
