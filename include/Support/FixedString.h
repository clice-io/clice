#pragma once

#include <array>
#include <string_view>

namespace clice {

template <std::size_t N>
struct fixed_string : std::array<char, N + 1> {
    template <std::size_t M>
    constexpr fixed_string(const char (&str)[M]) {
        for(std::size_t i = 0; i < N; ++i) {
            this->data()[i] = str[i];
        }
        this->data()[N] = '\0';
    }

    constexpr fixed_string(const char* str) {
        for(std::size_t i = 0; i < N; ++i) {
            this->data()[i] = str[i];
        }
        this->data()[N] = '\0';
    }

    constexpr auto size() const {
        return N;
    }

    constexpr operator std::string_view() const {
        return {this->data(), N};
    }
};

template <std::size_t M>
fixed_string(const char (&)[M]) -> fixed_string<M - 1>;

}  // namespace clice
