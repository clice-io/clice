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

class §(class_type)Store {};

struct App {
    Logger logger;
    Store store;
};

int use(App& app) {
    app.§(field)logger;
    app.§(class_field)store;
    return 0;
}
