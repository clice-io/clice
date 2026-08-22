/// - verify: server
///
/// Class and method names formed by token paste anchor at the
/// registration-macro invocation, the gtest-style pattern: typed uses of
/// the pasted name navigate there, and the base class lists the pasted
/// derived type in its subtypes.

struct §(base)TestBase {
    virtual void §(run_decl)run() = 0;
    virtual ~TestBase() = default;
};

#define REGISTER_TEST(suite, name)                                                                 \
    struct suite##_##name##_Test : TestBase {                                                      \
        void run() override;                                                                       \
    };                                                                                             \
    void suite##_##name##_Test::run()

§(register_site)REGISTER_TEST(Parser, HandlesEmpty) {
}

void drive() {
    static §(class_use)Parser_HandlesEmpty_Test instance;
    instance.§(run_use)run();
}
