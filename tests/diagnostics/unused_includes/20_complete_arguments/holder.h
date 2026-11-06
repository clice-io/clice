#pragma once
template <typename T>
struct Holder {
    T* pointer = nullptr;

    void reset(T* next) {
        static_assert(sizeof(T) > 0, "cannot delete an incomplete type");
        delete pointer;
        pointer = next;
    }
};
