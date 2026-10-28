struct MetaObject {};

#define OBJECT                                          \
public:                                                 \
    static const MetaObject staticMetaObject;           \
    virtual const MetaObject* metaObject() const;       \
                                                        \
private:                                                \
    static void static_metacall(int, void**);

class Window {
    OBJECT
public:
    void show();
};

#define TEST(suite, name)                               \
    class suite##_##name##_Test {                       \
    public:                                             \
        suite##_##name##_Test() = default;              \
                                                        \
    private:                                            \
        void TestBody();                                \
        static const int test_info_;                    \
    };                                                  \
    const int suite##_##name##_Test::test_info_ = 0;    \
    void suite##_##name##_Test::TestBody()

TEST(Math, Adds) {
    int sum = 1 + 1;
}

TEST(Math, Subtracts) {
    int difference = 2 - 1;
}

#define HANDLER(name) void name()

HANDLER(on_ready);

#define STRONG_TYPEDEF(base, name)                      \
    struct name {                                       \
        name() = default;                               \
        name(base raw) : value(raw) {}                  \
        base value;                                     \
    };

STRONG_TYPEDEF(int, Meters)
