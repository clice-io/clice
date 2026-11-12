// A conversion function has no return type to print, and a parameter whose type
// wraps its name keeps that shape.

struct Source {
    virtual ~Source() = default;
    virtual operator bool() const = 0;
    virtual void subscribe(void (*callback)(int, char)) = 0;
};

struct §(sink)Sink : Source {
};

struct Handlers {
    using Result = int;
    virtual Result (*handler(int x) const)(int) = 0;
};

struct §(pointer_return)Dispatcher : Handlers {
};
