/// # Conversion functions and pointer parameters
///
/// - status: supported
///
/// A conversion function has no return type to print, and a parameter whose type wraps its name keeps that shape

struct Source {
    virtual ~Source() = default;
    virtual operator bool() const = 0;
    virtual void subscribe(void (*callback)(int, char)) = 0;
};

struct §(sink)Sink : Source {
};
