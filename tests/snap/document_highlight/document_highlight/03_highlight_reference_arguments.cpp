/// # Arguments passed by mutable reference
///
/// - status: supported
///
/// An argument bound to a non-const lvalue reference parameter is a write,
/// where the `&` inlay hint marks it
///
/// A const reference or a by-value parameter reads the argument, and so does
/// taking its address. A forwarding reference (`T&&`, `auto&&`) binds
/// anything and reads the argument too, unless a variadic forwarder passes
/// it on to a mutable reference parameter.

namespace std {
template <class T> struct remove_reference { using type = T; };
template <class T> struct remove_reference<T&> { using type = T; };
template <class T>
T&& forward(typename remove_reference<T>::type& value) noexcept;
}

void reset(int& value);
void inspect(const int& value);
void copy(int value);
void exchange(int& lhs, int& rhs);
template <class T> void forward_one(T&& value);
void forward_auto(auto&& value);

struct Gauge {
    Gauge(int& source);
};

template <class T, class... Args>
T make(Args&&... args) {
    return T(std::forward<Args>(args)...);
}

void run() {
    int §(level)level = 0;
    int other = 1;
    reset(level);
    inspect(level);
    copy(level);
    exchange(level, other);
    forward_one(level);
    forward_auto(level);
    Gauge gauge(level);
    make<Gauge>(level);
    int* pointer = &level;
}
