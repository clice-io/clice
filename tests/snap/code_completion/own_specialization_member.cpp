// - diagnostics: expected

// Inside `Holder<T>`, a member of `Holder<W<T>>` binds the class's own `T`
// to `W<T>` exactly once, whether the member is declared `auto` from a call
// or a written type, or spelled out: all three complete the primary `W`, not
// one of the deeper partial specializations.
template <typename T>
struct W {
    static W<T> make();
    void w_primary();
};

template <typename T>
struct W<W<W<T>>> {
    void w_triple();
};

template <typename T>
struct W<W<W<W<T>>>> {
    void w_quad();
};

template <typename T>
struct Holder {
    static inline auto from_call = W<T>::make();
    static inline auto from_written = W<T>{};
    static inline W<T> plain;

    void f() {
        Holder<W<T>>::from_call.§(from_call);
        Holder<W<T>>::from_written.§(from_written);
        Holder<W<T>>::plain.§(plain);
    }
};
