// - flags: ["-fdelayed-template-parsing"]

// Clang defers template bodies on MSVC targets by default. The main file is
// parsed eagerly regardless, so the constructor body still folds and the
// request no longer crashes the worker (clice#693).
template <typename T>
union mini_variant_impl {
    T value;
    mini_variant_impl() {
    }
};
