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
