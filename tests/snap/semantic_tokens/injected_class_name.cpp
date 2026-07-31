/// # Conflict & Ambiguity
///
/// ## Injected class name — the class name used as a constructor call inside the class
///
/// - status: partial
/// - order: 2
///
/// The name itself renders as the class; the constructor reference
/// currently emits a stray token on the following `(`.

struct Widget {
    Widget(int size);

    Widget create() {
        return §Widget§()(42);
    }
};
