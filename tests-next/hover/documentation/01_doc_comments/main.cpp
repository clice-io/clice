namespace reference_docs {
/// Joins two counters.
long §(function)join(long left, long right);

/// Stores an ordinary value.
template <typename T> struct §(primary_def)Store {};

/// Stores an indirect value.
template <typename T> struct §(spec_def)Store<T*> {};

void inspect() {
    Sto§(primary_ref)re<long> ordinary;
    Sto§(spec_ref)re<long*> indirect;
}
}
