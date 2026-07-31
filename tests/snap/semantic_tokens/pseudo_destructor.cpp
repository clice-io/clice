/// # Token Correctness
///
/// ## Pseudo-destructor on a template parameter — the `~` paints nothing; the type name keeps its kind
///
/// - status: supported
/// - order: 6

template <typename T>
void reset(T* value) {
    value->§~§T();
}
