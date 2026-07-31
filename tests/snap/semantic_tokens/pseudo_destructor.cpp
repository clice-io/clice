/// # Token Correctness
///
/// ## Pseudo-destructor on a template parameter — nothing to resolve, nothing painted
///
/// - status: supported
/// - order: 6

template <typename T>
void reset(T* value) {
    value->§~§T();
}
