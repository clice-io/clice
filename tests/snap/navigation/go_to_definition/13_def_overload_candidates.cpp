/// # Dependent overload candidates
///
/// - status: supported
/// - verify: server
///
/// A dependent call that may reach several overloads lists each of them
///
/// Every candidate answers on its own: the definition where it has one, its
/// declaration where it has none.

void scale(int value);

void scale(double value) {}

template <typename T>
void apply(T value) {
    §(call)scale(value);
}
