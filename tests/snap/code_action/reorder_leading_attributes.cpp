// Attribute specifiers right before a definition move with it, on a line
// of their own or not, macro-spelled or not.

#define DEPRECATED [[deprecated]]

struct §(cls)S {
    void a();
    int b();
    void c();
};

[[nodiscard]] [[deprecated("old")]]
int S::b() {
    return 0;
}

[[using gnu: cold]] void S::c() {}

DEPRECATED
void S::a() {}
