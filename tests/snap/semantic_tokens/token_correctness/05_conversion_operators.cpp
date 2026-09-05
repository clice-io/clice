/// # Conversion operators
///
/// - status: supported
///
/// Written as keywords, converting uses paint nothing extra

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
