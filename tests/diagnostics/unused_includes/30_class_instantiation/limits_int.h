#pragma once
template <typename T>
struct Limits;

template <>
struct Limits<int> {
    static constexpr int size = 4;
};
