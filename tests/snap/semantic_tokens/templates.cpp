/// # Declarations & References
///
/// ## Templates — type and non-type template parameters, with the `templated` modifier on template names
///
/// - status: supported
/// - order: 5

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
