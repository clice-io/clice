/// # Specifiers of the override
///
/// - status: supported
///
/// A C variadic parameter, `consteval` and whether the base's method is `noexcept` carry over to the override
///
/// A method whose exception specification depends on the arguments of a base class template gets no declaration.

#define NOTHROW noexcept

namespace io {
constexpr bool quiet = true;

struct Logger {
    virtual void log(const char* format, ...) = 0;
    virtual void print(...) = 0;
    virtual void flush() NOTHROW = 0;
    virtual void sync() noexcept(quiet) = 0;
    virtual void rotate() noexcept(false) = 0;
    virtual consteval int level() = 0;
};

template <bool Quiet>
struct Channel {
    virtual void drain() noexcept(Quiet) = 0;
};
}

struct §(console)Console : io::Logger {};

struct §(channel)Pipe : io::Channel<true> {};
