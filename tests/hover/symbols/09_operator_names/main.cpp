struct Handle {};

struct Vec {
    int x;
    §(dtor_tilde)~§(dtor_class)Vec();
    bool §(eq_keyword)operator§(eq_symbol)==(const Vec& other) const;
    §(conv_keyword)operator §(conv_type)Handle*() const;
};

unsigned long long §(literal_keyword)operator""§(literal_suffix)_km(unsigned long long value);

namespace special_members {
struct Resource {
    §(destructor)~Resource();
};

struct Conversion {
    op§(conversion_operator)erator long();
};

class SelfConversion {
    operator §(conversion_target)SelfConversion();
};
}
