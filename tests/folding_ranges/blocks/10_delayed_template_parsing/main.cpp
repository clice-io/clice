// The command defers template bodies, so the constructor's body has no AST:
// it folds at its braces, and the request no longer crashes the worker on it
// (clice#693).
template <typename T>
union mini_variant_impl {
    T value;
    mini_variant_impl() {
    }
};
