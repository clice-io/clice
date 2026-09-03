/// # Pseudo-destructor on a template parameter — the `~` paints nothing; the type name keeps its kind
///
/// - status: supported

template <typename T>
void reset(T* value) {
    value->§~§T();
}
