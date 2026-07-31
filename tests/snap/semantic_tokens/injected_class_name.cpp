/// # Conflict & Ambiguity
///
/// ## Injected class name — the class name used as a constructor call inside the class
///
/// - status: supported
/// - order: 4
///
/// The written name renders as the class; the constructor reference it
/// implies paints nothing extra — the `(` stays token-free.

struct Widget {
    Widget(int size);

    Widget create() {
        return §Widget§()(42);
    }
};
