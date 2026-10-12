// Written as keywords, converting uses paint nothing extra.
struct Ratio {
    §(operator_use)operator double() const;
    explicit §(operator_use2)operator bool() const;
};

double to_double(Ratio ratio) {
    if (§(ratio_use)ratio) {
        return §(ratio_use2)ratio;
    }
    return double(§(ratio_use3)ratio);
}
