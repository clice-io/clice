/// # Doxygen `///` comments
///
/// - status: supported
///
/// Extracted from the declaration and rendered on hover
///
/// Applies to plain functions, primary templates and their specializations;
/// a reference resolves to the most specialized declaration's comment.

namespace reference_docs {
/// Joins two counters.
long §(01_function)join(long left, long right);

/// Stores an ordinary value.
template <typename T> struct §(02_primary_def)Store {};

/// Stores an indirect value.
template <typename T> struct §(03_spec_def)Store<T*> {};

void inspect() {
    Sto§(04_primary_ref)re<long> ordinary;
    Sto§(05_spec_ref)re<long*> indirect;
}
}
