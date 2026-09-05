/// # Pseudo-destructor on a template parameter
///
/// - status: supported
///
/// The `~` paints nothing; the type name keeps its kind

template <typename T>
void reset(T* value) {
    value->§~§T();
}
