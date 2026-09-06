/// # Predefined identifiers
///
/// - status: supported
///
/// `__func__` hover shows the current function name
///
/// The value resolves in a concrete function; inside a template only the
/// approximate type is known.

namespace predefined {

void current() {
    const char* name = __f§(func_name)unc__;
}

template <int N>
void generic() {
    const char* name = __f§(func_dependent)unc__;
}

}
