/// # Injected class name
///
/// - status: supported
///
/// An injected class name keeps its class token when used as a constructor
///
/// The written name renders as the class; the constructor reference it
/// implies paints nothing extra — the `(` stays token-free.

struct Widget {
    Widget(int size);

    Widget create() {
        return §Widget§()(42);
    }
};
