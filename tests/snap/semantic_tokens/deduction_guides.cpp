/// # Declarations & References
///
/// ## Deduction guides — the guide name and the guided template highlighted
///
/// - status: supported
/// - order: 14

template <typename T>
struct Vec {
    template <typename It>
    Vec(It first, It last);
};

template <typename It>
§Vec(It, It) -> §Vec<int>;
