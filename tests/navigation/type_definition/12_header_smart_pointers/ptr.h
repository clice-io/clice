#pragma once

template <typename T>
struct Ptr {
    T* operator->();
    T* raw;
};

struct Widget {
    int size;
};

extern Ptr<Widget> shared;
