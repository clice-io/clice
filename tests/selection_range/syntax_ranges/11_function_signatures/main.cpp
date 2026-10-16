struct Timer {
    Timer(int ticks);
    [[nodiscard]] long §(method)elapsed(int scale) const noexcept;
    const char* §(qualified)label() const;
};

Timer::§(constructor)Timer(int ticks) {}

auto §(trailing)tick(int step) -> long {
    return step;
}
