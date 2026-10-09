/// # Templates
///
/// - status: supported
///
/// Template parameters receive type or variable kinds, and template names carry
/// `templated`

template <typename §T, int §N>
struct §Array {
    §T data[§N];
};

template <typename T>
T §identity(T value);

template <typename §T>
§T §identity(§T value) {
    return value;
}

§Array<int, 4> arr;
int result = §identity(3);
