/// # Bases sharing a signature
///
/// - status: supported
///
/// One declaration overrides the pure virtual methods of every base with that signature, `noexcept` when any of them is

struct Reader {
    virtual void close() = 0;
    virtual void flush() noexcept = 0;
};

struct Writer {
    virtual void close() = 0;
    virtual void flush() = 0;
};

struct §(file)File : Reader, Writer {};
