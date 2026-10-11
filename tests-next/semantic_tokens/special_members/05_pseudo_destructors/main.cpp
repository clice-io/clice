// The `~` paints nothing; the type name keeps its kind.
template <typename T>
void reset(T* value) {
    value->§(tilde)~§(T)T();
}
