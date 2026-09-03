/// # Deduction guides — the guide name and the guided template highlighted
///
/// - status: supported

template <typename T>
struct Vec {
    template <typename It>
    Vec(It first, It last);
};

template <typename It>
§Vec(It, It) -> §Vec<int>;
