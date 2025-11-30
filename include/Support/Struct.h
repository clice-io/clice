#pragma once

#include <array>
#include <source_location>
#include <string_view>
#include <tuple>

#include "Support/TypeTraits.h"

namespace clice::refl {

namespace impl {

struct Any {
    consteval Any(std::size_t);

    template <typename T>
    consteval operator T() const;
};

template <typename T, std::size_t N>
consteval auto test() {
    return []<std::size_t... I>(std::index_sequence<I...>) {
        return requires { T{Any(I)...}; };
    }(std::make_index_sequence<N>{});
}

template <typename T, std::size_t N = 0>
consteval auto member_count() {
    if constexpr(test<T, N>() && !test<T, N + 1>()) {
        return N;
    } else {
        return member_count<T, N + 1>();
    }
}

template <typename T>
struct wrapper {
    T value;

    constexpr wrapper(T value) : value(value) {}
};

template <typename T>
union storage_t {
    char dummy;
    T value;

    storage_t() {}

    ~storage_t() {}
};

template <typename T>
inline storage_t<T> storage;

template <wrapper T>
consteval auto member_name() {
    std::string_view name = std::source_location::current().function_name();
#if __GNUC__ && (!__clang__) && (!_MSC_VER)
    std::size_t start = name.rfind("::") + 2;
    std::size_t end = name.rfind(')');
    name = name.substr(start, end - start);
#elif __clang__
    std::size_t start = name.rfind(".") + 1;
    std::size_t end = name.rfind('}');
    name = name.substr(start, end - start);
#elif _MSC_VER
    std::size_t start = name.rfind("->") + 2;
    std::size_t end = name.rfind('}');
    name = name.substr(start, end - start);
#else
    static_assert(false, "Not supported compiler");
#endif
    if(name.rfind("::") != std::string_view::npos) {
        name = name.substr(name.rfind("::") + 2);
    }
    return name;
}

template <std::size_t N>
constexpr inline auto to_string_literal_impl = [] {
    if constexpr(N == 0) {
        return std::array<char, 2>{'0', '\0'};
    } else {
        constexpr auto length = [] {
            std::size_t result = 0;
            for(std::size_t n = N; n; n /= 10) {
                ++result;
            }
            return result;
        }();

        std::array<char, length + 1> result = {};
        std::size_t n = N;
        for(std::size_t i = length; i; n /= 10, i--) {
            result[i - 1] = '0' + n % 10;
        }
        result[length] = '\0';
        return result;
    }
}();

}  // namespace impl

template <std::size_t N>
constexpr std::string_view to_string_literal() {
    return {impl::to_string_literal_impl<N>.data()};
}

template <typename T>
struct Struct;

/// To check if the type is reflectable_struct.
template <typename T>
concept reflectable_struct = Struct<std::remove_cvref_t<T>>::reflectable_struct;

/// Get the member count of the type.
template <typename T>
constexpr static std::size_t member_count() {
    return Struct<std::remove_cvref_t<T>>::member_count;
}

/// Get the all member names of the type.
template <typename T>
constexpr static auto& member_names() {
    return Struct<std::remove_cvref_t<T>>::member_names;
}

/// Get the member name of the type at index N.
template <typename T, std::size_t N>
constexpr static std::string_view member_name() {
    return member_names<T>()[N];
}

/// Get the member types of the type.
template <std::size_t N, typename T>
constexpr decltype(auto) member_value(T&& object) {
    return *std::get<N>(Struct<std::remove_cvref_t<T>>::collect_members(object));
}

template <typename T>
using member_types =
    tuple_to_list_t<decltype(Struct<T>::collect_members(std::declval<T>())), std::remove_pointer_t>;

template <typename T, std::size_t I>
using member_type = std::tuple_element_t<I, typename member_types<T>::to_tuple>;

template <typename T>
concept TupleLike = requires { std::tuple_size<T>::value; };

/// Specialize for aggregate class.
template <typename T>
    requires std::is_aggregate_v<T> && (!TupleLike<T>)
struct Struct<T> {
    constexpr inline static bool reflectable_struct = true;

    constexpr inline static auto member_count = impl::member_count<T>();

    template <typename Object>
    constexpr static auto collect_members(Object&& object) {
        // clang-format off
        if constexpr (member_count == 0) {
            return std::tuple{};
        } else if constexpr (member_count == 1) {
            auto&& [e1] = object;
                return std::tuple{ &e1 };
        } else if constexpr (member_count == 2) {
            auto&& [e1, e2] = object;
            return std::tuple{ &e1, &e2 };
        } else if constexpr (member_count == 3) {
            auto&& [e1, e2, e3] = object;
            return std::tuple{ &e1, &e2, &e3 };
        } else if constexpr (member_count == 4) {
            auto&& [e1, e2, e3, e4] = object;
            return std::tuple{ &e1, &e2, &e3, &e4 };
        } else if constexpr (member_count == 5) {
            auto&& [e1, e2, e3, e4, e5] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5 };
        } else if constexpr (member_count == 6) {
            auto&& [e1, e2, e3, e4, e5, e6] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6 };
        } else if constexpr (member_count == 7) {
            auto&& [e1, e2, e3, e4, e5, e6, e7] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7 };
        } else if constexpr (member_count == 8) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8 };
        } else if constexpr (member_count == 9) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9 };
        } else if constexpr (member_count == 10) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10 };
        } else if constexpr (member_count == 11) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11 };
        } else if constexpr (member_count == 12) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12 };
        } else if constexpr (member_count == 13) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13 };
        } else if constexpr (member_count == 14) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14 };
        } else if constexpr (member_count == 15) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15 };
        } else if constexpr (member_count == 16) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16 };
        } else if constexpr (member_count == 17) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17 };
        } else if constexpr (member_count == 18) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18 };
        } else if constexpr (member_count == 19) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19 };
        } else if constexpr (member_count == 20) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20 };
        } else if constexpr (member_count == 21) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20, &e21 };
        } else if constexpr (member_count == 22) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20, &e21, &e22 };
        } else if constexpr (member_count == 23) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20, &e21, &e22, &e23 };
        } else if constexpr (member_count == 24) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20, &e21, &e22, &e23, &e24 };
        } else if constexpr (member_count == 25) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24, e25] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20, &e21, &e22, &e23, &e24, &e25 };
        } else if constexpr (member_count == 26) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24, e25, e26] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20, &e21, &e22, &e23, &e24, &e25, &e26 };
        } else if constexpr (member_count == 27) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24, e25, e26, e27] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20, &e21, &e22, &e23, &e24, &e25, &e26, &e27 };
        } else if constexpr (member_count == 28) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24, e25, e26, e27, e28] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20, &e21, &e22, &e23, &e24, &e25, &e26, &e27, &e28 };
        } else if constexpr (member_count == 29) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24, e25, e26, e27, e28, e29] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20, &e21, &e22, &e23, &e24, &e25, &e26, &e27, &e28, &e29 };
        } else if constexpr (member_count == 30) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24, e25, e26, e27, e28, e29, e30] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20, &e21, &e22, &e23, &e24, &e25, &e26, &e27, &e28, &e29, &e30 };
        } else if constexpr (member_count == 31) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24, e25, e26, e27, e28, e29, e30, e31] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20, &e21, &e22, &e23, &e24, &e25, &e26, &e27, &e28, &e29, &e30, &e31 };
        } else if constexpr (member_count == 32) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24, e25, e26, e27, e28, e29, e30, e31, e32] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20, &e21, &e22, &e23, &e24, &e25, &e26, &e27, &e28, &e29, &e30, &e31, &e32 };
        } else if constexpr (member_count == 33) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24, e25, e26, e27, e28, e29, e30, e31, e32, e33] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20, &e21, &e22, &e23, &e24, &e25, &e26, &e27, &e28, &e29, &e30, &e31, &e32, &e33 };
        } else if constexpr (member_count == 34) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24, e25, e26, e27, e28, e29, e30, e31, e32, e33, e34] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20, &e21, &e22, &e23, &e24, &e25, &e26, &e27, &e28, &e29, &e30, &e31, &e32, &e33, &e34 };
        } else if constexpr (member_count == 35) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24, e25, e26, e27, e28, e29, e30, e31, e32, e33, e34, e35] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20, &e21, &e22, &e23, &e24, &e25, &e26, &e27, &e28, &e29, &e30, &e31, &e32, &e33, &e34, &e35 };
        } else if constexpr (member_count == 36) {
            auto&& [e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15, e16, e17, e18, e19, e20, e21, e22, e23, e24, e25, e26, e27, e28, e29, e30, e31, e32, e33, e34, e35, e36] = object;
            return std::tuple{ &e1, &e2, &e3, &e4, &e5, &e6, &e7, &e8, &e9, &e10, &e11, &e12, &e13, &e14, &e15, &e16, &e17, &e18, &e19, &e20, &e21, &e22, &e23, &e24, &e25, &e26, &e27, &e28, &e29, &e30, &e31, &e32, &e33, &e34, &e35, &e36 };
        } else {
            // For counts greater than 36, trigger a compile-time error
            static_assert(member_count <= 36, "Not supported member count");
        }
        // clang-format on
    }

    constexpr inline static auto member_names = []<std::size_t... Is>(std::index_sequence<Is...>) {
        if constexpr(member_count == 0) {
            return std::array<std::string_view, 1>{};
        } else {
            constexpr auto members = collect_members(impl::storage<T>.value);
            return std::array{impl::member_name<std::get<Is>(members)>()...};
        }
    }(std::make_index_sequence<member_count>{});
};

template <typename... Ts>
struct Inheritance : Ts... {};

/// Use to define a reflectable_struct struct with inheritance.
#define inherited_struct(name, ...)                                                                \
    struct name##Body;                                                                             \
    using name = clice::refl::Inheritance<__VA_ARGS__, name##Body>;                                \
    struct name##Body

template <typename... Ts>
struct Struct<Inheritance<Ts...>> {
    constexpr inline static bool reflectable_struct = (refl::reflectable_struct<Ts> && ...);

    constexpr static std::size_t member_count = (impl::member_count<Ts>() + ...);

    template <typename Object>
    constexpr static auto collect_members(Object&& object) {
        if constexpr(std::is_const_v<std::remove_reference_t<Object>>) {
            return std::tuple_cat(Struct<Ts>::collect_members(static_cast<const Ts&>(object))...);
        } else {
            return std::tuple_cat(Struct<Ts>::collect_members(static_cast<Ts&>(object))...);
        }
    }

    constexpr inline static auto member_names = []<std::size_t... Is>(std::index_sequence<Is...>) {
        if constexpr(member_count == 0) {
            return std::array<std::string_view, 1>{};
        } else {
            constexpr auto members = collect_members(impl::storage<Inheritance<Ts...>>.value);
            return std::array{impl::member_name<std::get<Is>(members)>()...};
        }
    }(std::make_index_sequence<member_count>{});
};

template <TupleLike T>
struct Struct<T> {
    constexpr inline static bool reflectable_struct = true;

    constexpr inline static std::size_t member_count = std::tuple_size_v<T>;

    template <typename Object>
    constexpr static auto collect_members(Object&& object) {
        return std::apply([](auto&&... args) { return std::tuple{&args...}; }, object);
    }

    constexpr inline static auto member_names = []<std::size_t... Is>(std::index_sequence<Is...>) {
        if constexpr(member_count == 0) {
            return std::array<std::string_view, 1>{};
        } else {
            return std::array{to_string_literal<Is>()...};
        }
    }(std::make_index_sequence<member_count>{});
};

/// Turn the return value of the callable to bool.
template <typename Callable>
constexpr auto foldable(const Callable& callable) {
    return [&](auto&&... args) {
        using Ret = std::invoke_result_t<Callable, decltype(args)...>;
        if constexpr(std::is_void_v<Ret>) {
            callable(args...);
            return true;
        } else {
            return bool(callable(args...));
        }
    };
}

template <reflectable_struct Object, typename Callback>
constexpr bool foreach(Object&& object, const Callback& callback) {
    auto foldable = refl::foldable(callback);
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        return (foldable(refl::member_name<Object, Is>(), refl::member_value<Is>(object)) && ...);
    }(std::make_index_sequence<refl::member_count<Object>()>{});
}

/// Invoke callback for each member of lhs and rhs, return false
/// in callback to abort the iteration. Return true if all members are visited.
template <reflectable_struct LHS, reflectable_struct RHS, typename Callback>
constexpr bool foreach(LHS&& lhs, RHS&& rhs, const Callback& callback) {
    static_assert(member_count<LHS>() == member_count<RHS>(), "Member count mismatch");
    auto foldable = refl::foldable(callback);
    return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        return (foldable(refl::member_value<Is>(lhs), refl::member_value<Is>(rhs)) && ...);
    }(std::make_index_sequence<refl::member_count<LHS>()>{});
}

}  // namespace clice::refl
