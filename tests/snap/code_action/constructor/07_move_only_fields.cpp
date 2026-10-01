/// # Move-only fields
///
/// - status: supported
///
/// A field whose class moves but does not copy is taken by value and moved from, and an rvalue reference field binds its argument through `std::move`
///
/// The file gains `#include <utility>` when nothing declares `std::move`
/// before the class. A class that neither copies nor moves gets no
/// constructor.

struct Handle {
    Handle(Handle&&) = default;
};

struct §(owner)Owner {
    Handle handle;
    int&& pending;
    int count;
};

struct Lock {
    Lock(const Lock&) = delete;
};

struct §(pinned)Pinned {
    Lock lock;
    int count;
};

template <class T>
struct Slot {
    Slot(Slot&&) = default;
    Slot(const Slot&) = delete;
};

template <class T>
struct §(holder)Holder {
    Slot<T> slot;
    int count;
};
