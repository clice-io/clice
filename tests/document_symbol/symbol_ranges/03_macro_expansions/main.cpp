static_assert(true);

#define DEFINE_HANDLER(name) void name()

DEFINE_HANDLER(on_ready);
DEFINE_HANDLER(on_close);

#define DECLARE_CLASS(X) class X
DECLARE_CLASS(Generated) {
    int member;
};
