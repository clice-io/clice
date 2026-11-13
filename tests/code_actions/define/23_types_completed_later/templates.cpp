template <class T>
struct Later;

struct Pending {
    void §(never_defined)take(Later<int> later);
};

template <class T>
struct Soon;

struct Waiting {
    void §(defined_after)take(Soon<int> soon);
};

template <class T>
struct Soon {};

template <class F>
struct Function;

template <class R, class... Args>
struct Function<R(Args...)> {};

struct Widget {
    void §(partial_specialization)set(Function<void()> callback);
};
