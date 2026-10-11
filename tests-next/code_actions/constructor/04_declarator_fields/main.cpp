// A field whose type wraps its name, such as a function pointer or a reference
// to an array, keeps that shape in the parameter.

struct §(callbacks)Callbacks {
    void (*on_open)(int);
    int (&counter)[4];
    const char* name;
};
