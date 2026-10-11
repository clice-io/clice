// One declaration overrides the pure virtual methods of every base with that
// signature, `noexcept` when any of them is. The static_assert fails until
// the action leaves no method pure.

struct Reader {
    virtual void close() = 0;
    virtual void flush() noexcept = 0;
};

struct Writer {
    virtual void close() = 0;
    virtual void flush() = 0;
};

struct §(file)File : Reader, Writer {};

static_assert(!__is_abstract(File));
