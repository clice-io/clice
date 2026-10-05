// The conditional opens among the file's leading directives, which a
// server compiles into its preamble: the definitions inside it still
// reorder only among themselves.

#if !defined(FEATURE_OFF)
struct §(cls)S {
    void a();
    void b();
    void c();
};

void S::c() {}
void S::b() {}
#endif

void S::a() {}
