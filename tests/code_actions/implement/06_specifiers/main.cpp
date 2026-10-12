// A C variadic parameter, `consteval` and whether the base's method is
// `noexcept`, through a macro or a constant, carry over to the override. A
// method whose exception specification depends on the arguments of a base
// class template gets no declaration. The static_assert fails until the
// action leaves no method pure.

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

static_assert(!__is_abstract(Console));

struct §(channel)Pipe : io::Channel<true> {};
