template <typename T>
union mini_variant_impl {
    T value;
    mini_variant_impl() {
    }
};
