/// # Concepts — definitions and uses as template constraints
///
/// - status: supported

template <typename T>
concept §Small = sizeof(T) <= 4;

template <§Small T>
void use_small(T value);

template <typename T>
    requires §Small<T>
void require_small(T value);
