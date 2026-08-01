/// # Fold Kinds
///
/// ## Initializer-list constructions — the constructor's braces and the nested initializer list share delimiters and fold once; a parenthesized list argument keeps both folds
///
/// - status: supported
/// - order: 17

namespace std {

template <typename T>
class initializer_list {
public:
    using size_type = decltype(sizeof(0));

    const T* ptr = nullptr;
    size_type len = 0;
};

}  // namespace std

struct Bag {
    Bag(std::initializer_list<int> values);
};

Bag braces{
    1,
    2
};

Bag nested({
    3,
    4
});
