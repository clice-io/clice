/// # Go to Type Definition
///
/// ## Class and struct fields
///
/// - status: supported
/// - verify: server
/// - order: 2
///
/// Go-to-type-definition on a field access reaches the definition of the
/// field's type.

struct §(type)Logger {};

struct App {
    Logger logger;
};

int use(App& app) {
    app.§(field)logger;
    return 0;
}
