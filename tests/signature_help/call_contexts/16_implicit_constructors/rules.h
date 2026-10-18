#pragma once

struct Base {
    Base();
    Base(const Base& other);
    Base(Base&& other);
};

template <class T>
struct With : Base {
    T meta;
};

template <>
struct With<void> : Base {};

using Rule = With<void>;

// Copying and moving a holder declare the copy and the move constructor of
// Rule, before anything needs its default constructor.
struct Holder {
    Rule rule;
};

inline Holder copied(const Holder& holder) {
    return holder;
}

inline Holder moved(Holder holder) {
    return static_cast<Holder&&>(holder);
}
