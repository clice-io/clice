/// # Specifiers of the override
///
/// - status: supported
///
/// A C variadic parameter, `consteval` and the exception specification the base's type has carry over to the override
///
/// An exception specification that depends on the arguments of a base class template is not known before the compiler needs it, and such a method gets no declaration.

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
