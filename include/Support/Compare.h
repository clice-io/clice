#pragma once

#include <vector>

#include "Enum.h"
#include "Struct.h"

namespace clice::refl {

template <typename LHS, typename RHS = LHS>
struct Equal {
    constexpr static bool equal(const LHS& lhs, const RHS& rhs) {
        return lhs == rhs;
    }
};

struct equal_t {
    template <typename LHS, typename RHS = LHS>
    constexpr static bool operator()(const LHS& lhs, const RHS& rhs) {
        return Equal<LHS, RHS>::equal(lhs, rhs);
    }
};

constexpr inline equal_t equal;

template <typename T>
struct Equal<std::vector<T>> {
    constexpr static bool equal(const std::vector<T>& lhs, const std::vector<T>& rhs) {
        if(lhs.size() != rhs.size()) {
            return false;
        }

        for(std::size_t i = 0; i < lhs.size(); ++i) {
            if(!refl::equal(lhs[i], rhs[i])) {
                return false;
            }
        }

        return true;
    }
};

template <reflectable_enum E>
struct Equal<E> {
    constexpr static bool equal(E lhs, E rhs) {
        return lhs.value() == rhs.value();
    }
};

template <reflectable_struct T>
    requires (!requires(T lhs, T rhs) {
        { lhs == rhs } -> std::convertible_to<bool>;
    })
struct Equal<T> {
    constexpr static bool equal(const T& lhs, const T& rhs) {
        return foreach(lhs, rhs, [](const auto& lhs, const auto& rhs) {
            return refl::equal(lhs, rhs);
        });
    }
};

template <typename RHS, typename LHS = RHS>
struct Less {
    constexpr static bool less(const LHS& lhs, const RHS& rhs) {
        return lhs < rhs;
    }
};

struct less_t {
    template <typename RHS, typename LHS = RHS>
    constexpr static bool operator()(const LHS& lhs, const RHS& rhs) {
        return Less<RHS, LHS>::less(lhs, rhs);
    }
};

constexpr inline less_t less;

template <typename T>
struct Less<std::vector<T>> {
    constexpr static bool less(const std::vector<T>& lhs, const std::vector<T>& rhs) {
        if(lhs.size() != rhs.size()) {
            return lhs.size() < rhs.size();
        }

        for(std::size_t i = 0; i < lhs.size(); ++i) {
            if(refl::less(lhs[i], rhs[i])) {
                return true;
            }
        }

        return false;
    }
};

template <reflectable_enum E>
struct Less<E> {
    constexpr static bool less(E lhs, E rhs) {
        return lhs.value() < rhs.value();
    }
};

template <reflectable_struct T>
    requires (!requires(T lhs, T rhs) {
        { lhs < rhs } -> std::convertible_to<bool>;
    })
struct Less<T> {
    constexpr static bool less(const T& lhs, const T& rhs) {
        bool result = false;
        foreach(lhs, rhs, [&](const auto& lhs, const auto& rhs) {
            /// return false to break the loop.
            if(refl::less(lhs, rhs)) {
                result = true;
                return false;
            }

            if(refl::less(rhs, lhs)) {
                result = false;
                return false;
            }

            /// continue the loop.
            return true;
        });
        return result;
    }
};

struct less_equal_t {
    template <typename LHS, typename RHS = LHS>
    constexpr static bool operator()(const LHS& lhs, const RHS& rhs) {
        return equal(lhs, rhs) || less(lhs, rhs);
    }
};

constexpr inline less_equal_t less_equal;

}  // namespace clice::refl
