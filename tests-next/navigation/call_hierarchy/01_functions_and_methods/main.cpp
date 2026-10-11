struct Service {
    void §(method)start();
};

void Service::start() {}

void §(func)launch(Service& s) {
    s.start();
}
