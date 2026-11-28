export module text;

export struct Narrow {
    using unit = char;
};

export struct Wide {
    using unit = char8_t;
};

export template <typename T>
struct Text;

template <>
struct Text<char> {
    using view = Narrow;
};

template <>
struct Text<char8_t> {
    using view = Wide;
};
