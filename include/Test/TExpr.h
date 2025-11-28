#pragma once

#include <string>
#include <format>
#include <algorithm>
#include <functional>
#include "Support/Compare.h"
#include "Support/Ranges.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"

namespace clice::testing {

template <typename T>
concept is_expr_v = requires { typename T::expr_tag; };

template <typename Derived>
struct default_formatter : std::formatter<std::string_view> {
    using Base = std::formatter<std::string_view>;

    template <typename FormatContext>
    auto format(const auto& value, FormatContext& ctx) const {
        llvm::SmallString<256> buffer;
        static_cast<const Derived*>(this)->format_to(std::back_inserter(buffer), value);
        return Base::format(std::string_view(buffer), ctx);
    }
};

template <typename Expr>
decltype(auto) compute(const Expr& expr) {
    if constexpr(requires { typename Expr::expr_tag; }) {
        return expr();
    } else {
        return expr;
    }
}

}  // namespace clice::testing

#define BINARY_PREDICATE(name, op)                                                                 \
    namespace clice::testing {                                                                     \
                                                                                                   \
    decltype(auto) name##_impl(auto&& lhs, auto&& rhs);                                            \
                                                                                                   \
    template <typename LHS, typename RHS>                                                          \
    struct name {                                                                                  \
        const LHS& lhs;                                                                            \
        const RHS& rhs;                                                                            \
                                                                                                   \
        using expr_tag = int;                                                                      \
                                                                                                   \
        auto operator() () const {                                                                 \
            return name##_impl(compute(lhs), compute(rhs));                                        \
        }                                                                                          \
    };                                                                                             \
                                                                                                   \
    template <typename LHS, typename RHS>                                                          \
    name(const LHS&, const RHS&) -> name<LHS, RHS>;                                                \
                                                                                                   \
    }                                                                                              \
                                                                                                   \
    template <typename LHS, typename RHS>                                                          \
    struct std::formatter<clice::testing::name<LHS, RHS>> :                                        \
        clice::testing::default_formatter<std::formatter<clice::testing::name<LHS, RHS>>> {        \
        void format_to(auto&& inserter, const auto& expr) const {                                  \
            std::format_to(inserter, "{} " #op " {}", expr.lhs, expr.rhs);                         \
        }                                                                                          \
    };                                                                                             \
                                                                                                   \
    decltype(auto) clice::testing::name##_impl(auto&& lhs, auto&& rhs)

BINARY_PREDICATE(add, +) {
    return lhs + rhs;
};

BINARY_PREDICATE(sub, -) {
    return lhs - rhs;
}

BINARY_PREDICATE(mul, *) {
    return lhs * rhs;
}

BINARY_PREDICATE(eq, ==) {
    return refl::equal(lhs, rhs);
}

BINARY_PREDICATE(ne, !=) {
    return !refl::equal(lhs, rhs);
}

BINARY_PREDICATE(lt, <) {
    return refl::less(lhs, rhs);
}

BINARY_PREDICATE(le, <=) {
    return refl::less_equal(lhs, rhs);
}

BINARY_PREDICATE(gt, >) {
    return refl::less(rhs, lhs);
}

BINARY_PREDICATE(ge, >=) {
    return refl::less_equal(rhs, lhs);
}

BINARY_PREDICATE(has, has) {
    return ranges::contains(lhs, rhs);
}

#undef BINARY_PREDICATE
