// Deduction guides and their guided templates receive type tokens.
template <typename T>
struct Vec {
    template <typename It>
    Vec(It first, It last);
};

template <typename It>
§(Vec_decl)Vec(It, It) -> §(Vec_use)Vec<int>;
