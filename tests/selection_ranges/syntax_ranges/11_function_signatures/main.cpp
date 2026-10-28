struct Clock {
    virtual long now() const = 0;
};

struct Timer : Clock {
    Timer(int ticks);
    [[nodiscard]] long §(method)elapsed(int scale) const noexcept;
    const char* §(qualified)label() const;
    long §(override)now() const override { return 0; }
};

Timer::§(constructor)Timer(int ticks) {}

auto §(trailing)tick(int step) -> long {
    return step;
}

template <typename T>
T §(constrained)twice(T value) requires (sizeof(T) > 1) {
    return value + value;
}
