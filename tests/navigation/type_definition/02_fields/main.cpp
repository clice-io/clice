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
