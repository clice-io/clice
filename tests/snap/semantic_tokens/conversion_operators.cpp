/// # Token Correctness
///
/// ## Conversion operators — written as keywords, converting uses paint nothing extra
///
/// - status: supported
/// - order: 5

struct Ratio {
    §operator double() const;
    explicit §operator bool() const;
};

double to_double(Ratio ratio) {
    if (§ratio) {
        return §ratio;
    }
    return double(§ratio);
}
