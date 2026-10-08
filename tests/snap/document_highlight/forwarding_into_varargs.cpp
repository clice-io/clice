// A variadic forwarder passing its pack on to a C variadic constructor:
// the pack's arguments reach past the constructor's named parameters, so
// the forwarding resolves to nothing and the arguments read.

struct Packet {
    Packet(int size, int& tag, ...);
};

template <class... Args>
Packet build(Args&... args) {
    return Packet(0, args...);
}

int §(first)first;
int second;
Packet packet = build(first, second);
