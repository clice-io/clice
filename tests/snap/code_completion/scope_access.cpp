/// # Member Access
///
/// ## Scope-qualified members — after `::` static data, nested types, methods and the injected class name all list
///
/// - status: supported
/// - order: 11
///
/// Qualified completion is not filtered to the statically-reachable subset:
/// instance fields and the destructor show up alongside the static members
/// and nested types.

// error-ok: the qualified-id is left dangling at the point.
struct Config {
    static int shared_count;
    static int make(int seed);

    struct Nested {
        int a;
    };

    int instance_field;
};

void bar() {
    int v = Config::§(pos);
}
