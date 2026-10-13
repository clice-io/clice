struct Callback {
    void operator()(int status, int detail) const;
};

void (*handler)(int status, const char* message);

struct Device {
    int (*open)(const char* path, int flags);
};

void use(Device* device) {
    Callback cb;
    cb(1, 2);
    cb.operator()(3, 4);
    handler(0, "ok");
    device->open("/dev/null", 0);
    auto cmp = [](int lhs, int rhs) { return lhs < rhs; };
    cmp(1, 2);
}
