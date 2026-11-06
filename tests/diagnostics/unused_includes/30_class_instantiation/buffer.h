#pragma once
template <typename T>
struct Limits;

template <typename T>
struct Buffer {
    char data[Limits<T>::size];
};
