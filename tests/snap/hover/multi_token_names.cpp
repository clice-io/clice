struct Handle {};

struct Vec {
    int x;
    §(01_dtor_tilde)~§(02_dtor_class)Vec();
    bool §(03_eq_keyword)operator§(04_eq_symbol)==(const Vec& other) const;
    §(05_conv_keyword)operator §(06_conv_type)Handle*() const;
};

unsigned long long §(07_literal_keyword)operator""§(08_literal_suffix)_km(unsigned long long value);
