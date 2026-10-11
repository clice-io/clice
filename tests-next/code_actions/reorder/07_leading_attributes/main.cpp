// Attribute specifiers right before a definition move with it, on a line
// of their own or not, macro-spelled or not.

#define DEPRECATED [[deprecated]]
#define OBSOLETE(reason) [[deprecated(reason)]]

struct §(cls)S {
    void a();
    int b();
    void c();
    void d();
};

[[nodiscard]] [[deprecated("old")]]
int S::b() {
    return 0;
}

[[using gnu: cold]] void S::c() {}

OBSOLETE("old")
void S::d() {}

DEPRECATED
void S::a() {}
