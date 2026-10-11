// A forwarding reference binds anything and reads its argument, unless a
// variadic forwarder passes it on to a mutable reference parameter. A pack
// passed on to a C variadic callee reaches past its named parameters and
// resolves to nothing; a forwarder whose other calls cannot carry the pack
// still resolves the one that does.

namespace std {
template <class T> struct remove_reference { using type = T; };
template <class T> struct remove_reference<T&> { using type = T; };
template <class T>
T&& forward(typename remove_reference<T>::type& value) noexcept;
}

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
    forward_one(level);
    forward_auto(level);
    make<Gauge>(level);
}

namespace varargs {

struct Packet {
    Packet(int size, int& tag, ...);
};

template <class... Args>
Packet build(Args&... args) {
    return Packet(0, args...);
}

int §(varargs)first;
int second;
Packet packet = build(first, second);

}

namespace short_calls {

struct Tag {
    Tag(int code);
};

void note(int code);
void fill(int& a, int& b, int& c);

template <class... Args>
void relay(Args&... args) {
    note(0);
    Tag tag(1);
    fill(args...);
}

int §(short_calls)first;
int second;
int third;

void use() {
    relay(first, second, third);
}

}
