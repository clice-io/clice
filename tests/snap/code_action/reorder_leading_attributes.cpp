// An attribute specifier on a line of its own above a definition moves
// with it, macro-spelled or not.

#define DEPRECATED [[deprecated]]

struct §(cls)S {
    void a();
    void b();
    void c();
};

void S::c() {}

[[nodiscard]] [[deprecated("old")]]
void S::b() {}

DEPRECATED
void S::a() {}
