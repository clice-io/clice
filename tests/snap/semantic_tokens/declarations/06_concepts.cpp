/// # Concepts
///
/// - status: supported
///
/// Concept definitions and constraint uses receive concept tokens

template <typename T>
concept §Small = sizeof(T) <= 4;

template <§Small T>
void use_small(T value);

template <typename T>
    requires §Small<T>
void require_small(T value);
