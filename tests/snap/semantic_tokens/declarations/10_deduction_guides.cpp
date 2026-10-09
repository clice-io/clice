/// # Deduction guides
///
/// - status: supported
///
/// Deduction guides and their guided templates receive type tokens

template <typename T>
struct Vec {
    template <typename It>
    Vec(It first, It last);
};

template <typename It>
§Vec(It, It) -> §Vec<int>;
